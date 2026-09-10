#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/frameworks/gpu/system_image_runtime.h"

#include <cuda_runtime_api.h>

#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
#include <vector>
#include <stdexcept>
#include <utility>

#include "src/controller/subsystems/annotation/detail/annotation_document.h"
#include "src/frameworks/gpu/cuda_high_water_allocation.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/backend/imaging/raster/detail/raster_color.h"

import mmltk.backend.imaging.raster;

namespace mmltk::controller {
namespace {

namespace raster = mmltk::backend::imaging::raster;
namespace document = mmltk::controller::subsystems::annotation;
namespace domain = mmltk::controller::contracts;

}  // namespace

namespace {

class NativeAnnotationAlgorithm final : public AnnotationAlgorithm {
   public:
    [[nodiscard]] AnnotationOperationResult Open(const mmltk::frameworks::gpu::ImagePlaneView source, domain::AnnotationSceneContent scene,
                                                 const VisualRegion crop) override {
        auto result = Result(document_.Open(std::move(scene)));
        if (result.outcome == AnnotationOperationOutcome::Applied) {
            crop_ = crop;
            source_ = source;
            for (auto& geometry : geometry_) geometry.scene_revision = 0U;
        }
        return result;
    }
    Release ReleaseResources() noexcept override {
        if (mask_upload_ != nullptr) {
            if (cudaEventDestroy(mask_upload_) != cudaSuccess) return {.all_released = false, .failure = release_failure_};
            mask_upload_ = nullptr;
        }
        const auto device = mask_device_.ReleaseAll([](void* p) { return cudaFree(p); });
        const auto sample = sample_host_ ? sample_host_->ReleaseSettled() : CUDA_SUCCESS;
        const auto host = mask_host_ ? mask_host_->ReleaseSettled() : CUDA_SUCCESS;
        if (!device.released() || host != CUDA_SUCCESS || sample != CUDA_SUCCESS)
            return {.all_released = false, .failure = release_failure_};
        return {};
    }

    [[nodiscard]] AnnotationPointerResult Pointer(const AnnotationPointer& pointer) override {
        const auto before = document_.ui().scene_revision;
        const auto pointer_result = [&](document::DocumentResult result) {
            return AnnotationPointerResult{
                .detail = std::move(result.detail),
                .outcome = result.outcome == document::DocumentOutcome::Applied ? AnnotationOperationOutcome::Applied
                                                                              : AnnotationOperationOutcome::Rejected,
                .ui_changed = document_.ui().scene_revision != before,
            };
        };
        auto result = document_.Pointer(pointer);
        if (result.outcome != document::DocumentOutcome::Applied || pointer.phase != domain::AnnotationPointerPhase::End ||
            document_.ui().editor.tool != domain::AnnotationTool::ColorSample ||
            !document_.ToolAvailable(domain::AnnotationTool::ColorSample, pointer.target.object))
            return pointer_result(std::move(result));
        if (!sample_host_) sample_host_ = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
        sample_host_->ensure_bytes(4);
        const auto x =
            std::min(static_cast<unsigned>(pointer.point.x), static_cast<unsigned>(document_.ui().scene.frame_width) - 1) + crop_.x;
        const auto y =
            std::min(static_cast<unsigned>(pointer.point.y), static_cast<unsigned>(document_.ui().scene.frame_height) - 1) + crop_.y;
        if (cudaMemcpy(sample_host_->data(), reinterpret_cast<const void*>(source_.data + y * source_.descriptor.pitch_bytes + x * 4U), 4,
                       cudaMemcpyDeviceToHost) != cudaSuccess)
            throw std::runtime_error("Annotation color sample failed");
        const auto* pixel = static_cast<const std::uint8_t*>(sample_host_->data());
        const float r = pixel[0] / 255.0F, g = pixel[1] / 255.0F, b = pixel[2] / 255.0F;
        const float high = std::max({r, g, b}), low = std::min({r, g, b}), delta = high - low;
        float hue = 0;
        if (delta > 0) {
            if (high == r)
                hue = 60 * std::fmod((g - b) / delta, 6.0F);
            else if (high == g)
                hue = 60 * ((b - r) / delta + 2);
            else
                hue = 60 * ((r - g) / delta + 4);
            if (hue < 0) hue += 360;
        }
        const auto& object = document_.ui().scene.objects[*pointer.target.object];
        auto supported = object.sup;
        supported.center = {hue, high == 0 ? 0 : delta / high, high};
        supported.sampling = true;
        auto selected = document_.Edit({.value = AnnotationObjectEdit{*pointer.target.object}});
        if (selected.outcome != document::DocumentOutcome::Applied) return pointer_result(std::move(selected));
        return pointer_result(document_.Edit({.value = AnnotationMaskColorsEdit{supported, object.nosup}}));
    }
    [[nodiscard]] const domain::AnnotationUiState& Ui() const noexcept override { return document_.ui(); }
    void PeerClosed() noexcept override { document_.PeerClosed(); }

    [[nodiscard]] AnnotationOperationResult Edit(const AnnotationEdit& edit) override { return Result(document_.Edit(edit)); }

    [[nodiscard]] AnnotationOperationResult Save(const std::string_view destination) override {
        return Result(document_.Save(destination));
    }

   private:
    [[nodiscard]] AnnotationOperationResult Result(document::DocumentResult result) const {
        return {
            .ui = document_.ui(),
            .detail = std::move(result.detail),
            .outcome = result.outcome == document::DocumentOutcome::Applied ? AnnotationOperationOutcome::Applied
                                                                            : AnnotationOperationOutcome::Rejected,
        };
    }

    void Render(const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView clean,
                const mmltk::frameworks::gpu::ImagePlaneView semantic, const std::uintptr_t stream_value) const override {
        auto stream = reinterpret_cast<cudaStream_t>(stream_value);
        cudaError_t status = source.valid() ? cudaMemcpy2DAsync(
            reinterpret_cast<void*>(clean.data), clean.descriptor.pitch_bytes,
            reinterpret_cast<const void*>(source.data + crop_.y * source.descriptor.pitch_bytes + crop_.x * 4U),
            // CLEANUP-IGNORE: Annotation copies its private source before semantic rendering.
            source.descriptor.pitch_bytes, clean.descriptor.row_bytes(), clean.descriptor.height, cudaMemcpyDeviceToDevice, stream) : cudaSuccess;
        if (status == cudaSuccess)
            status = cudaMemset2DAsync(reinterpret_cast<void*>(semantic.data), semantic.descriptor.pitch_bytes, 0,
                                       semantic.descriptor.row_bytes(), semantic.descriptor.height, stream);
        if (status != cudaSuccess) throw std::runtime_error("Annotation image plane preparation failed");
        const auto& scene = document_.ui().scene;
        // Reuse pinned staging only after its prior transfer has consumed it;
        // the rendering kernels themselves remain ordered on the runtime stream.
        if (mask_upload_ != nullptr && cudaEventSynchronize(mask_upload_) != cudaSuccess)
            throw std::runtime_error("Annotation mask upload settlement failed");
        std::size_t run_count = 0U;
        for (std::size_t index = 0; index < document_.RenderObjectCount(); ++index) {
            const auto& object = document_.RenderObjectAt(index);
            if (object.enabled) run_count += object.mask.runs.size();
        }
        if (geometry_.size() < document_.RenderObjectCount()) geometry_.resize(document_.RenderObjectCount());
        geometry_words_.clear();
        for (std::size_t index = 0; index < document_.RenderObjectCount(); ++index) {
            const auto& object = document_.RenderObjectAt(index);
            if (!object.enabled) continue;
            auto& geometry = geometry_[index];
            const bool preview = index >= scene.objects.size() || &object != &scene.objects[index];
            if (geometry.scene_revision != document_.ui().scene_revision || geometry.preview || preview) {
                geometry.words.clear();
                geometry.count = geometry.edges = geometry.handles = 0;
                geometry.edge_offset = geometry.handle_offset = 0U;
                const auto append_point = [&](domain::AnnotationPoint value) {
                    geometry.words.push_back(static_cast<std::uint32_t>(std::lround(value.x)));
                    geometry.words.push_back(static_cast<std::uint32_t>(std::lround(value.y)));
                };
                const auto point = [&](domain::AnnotationPoint value) {
                    append_point(value);
                    ++geometry.count;
                };
                if (object.shape == domain::AnnotationShape::Point) point(object.point);
                if (object.shape == domain::AnnotationShape::Spline && !object.spline_knots.empty()) {
                    point(object.spline_knots.front().point);
                    const auto segments = object.spline_knots.size() - (object.spline_closed ? 0U : 1U);
                    for (std::size_t segment = 0; segment < segments; ++segment) {
                        const auto& a = object.spline_knots[segment];
                        const auto& b = object.spline_knots[(segment + 1) % object.spline_knots.size()];
                        const auto c1 = a.out.enabled ? a.out.point : a.point;
                        const auto c2 = b.in.enabled ? b.in.point : b.point;
                        for (unsigned sample = 1; sample <= 16; ++sample) {
                            const float t = static_cast<float>(sample) / 16, u = 1 - t;
                            point({u * u * u * a.point.x + 3 * u * u * t * c1.x + 3 * u * t * t * c2.x + t * t * t * b.point.x,
                                   u * u * u * a.point.y + 3 * u * u * t * c1.y + 3 * u * t * t * c2.y + t * t * t * b.point.y});
                        }
                    }
                }
                if (object.shape == domain::AnnotationShape::Skeleton) {
                    for (const auto& node : object.skeleton_nodes)
                        point(node.point);
                    geometry.edge_offset = geometry.words.size();
                    for (const auto edge : object.skeleton_edges) {
                        if (!object.skeleton_nodes[edge.source].visible || !object.skeleton_nodes[edge.target].visible) continue;
                        geometry.words.push_back(edge.source);
                        geometry.words.push_back(edge.target);
                        ++geometry.edges;
                    }
                }
                geometry.handle_offset = geometry.words.size();
                const auto handle = [&](domain::AnnotationPoint value) {
                    append_point(value);
                    ++geometry.handles;
                };
                if (object.shape == domain::AnnotationShape::Skeleton)
                    for (const auto& node : object.skeleton_nodes)
                        if (node.visible) handle(node.point);
                if (document_.ui().editor.selected_object == index && object.shape == domain::AnnotationShape::Spline)
                    for (const auto& knot : object.spline_knots) {
                        handle(knot.point);
                        if (knot.in.enabled) handle(knot.in.point);
                        if (knot.out.enabled) handle(knot.out.point);
                    }
                geometry.scene_revision = document_.ui().scene_revision;
                geometry.preview = preview;
            }
            geometry.offset = geometry_words_.size();
            geometry_words_.insert(geometry_words_.end(), geometry.words.begin(), geometry.words.end());
        }
        const auto total_words = run_count * 2 + geometry_words_.size();
        if (total_words != 0U) {
            EnsureMasks(total_words * sizeof(std::uint32_t));
            auto* const pairs = static_cast<std::uint32_t*>(mask_host_->data());
            std::size_t offset = 0U;
            for (std::size_t index = 0; index < document_.RenderObjectCount(); ++index) {
                const auto& object = document_.RenderObjectAt(index);
                if (!object.enabled) continue;
                for (const auto run : object.mask.runs) {
                    pairs[offset++] = static_cast<std::uint32_t>(run.row) * clean.descriptor.width + run.first;
                    pairs[offset++] = static_cast<std::uint32_t>(run.last) - run.first + 1U;
                }
            }
            std::copy(geometry_words_.begin(), geometry_words_.end(), pairs + run_count * 2);
            if (cudaMemcpyAsync(mask_device_.active(), pairs, total_words * sizeof(std::uint32_t), cudaMemcpyHostToDevice, stream) !=
                cudaSuccess)
                throw std::runtime_error("Annotation mask upload failed");
            if (mask_upload_ == nullptr && cudaEventCreateWithFlags(&mask_upload_, cudaEventDisableTiming) != cudaSuccess)
                throw std::runtime_error("Annotation mask upload event creation failed");
            if (cudaEventRecord(mask_upload_, stream) != cudaSuccess)
                throw std::runtime_error("Annotation mask upload event recording failed");
        }
        std::size_t offset = 0U;
        if (palette_source_ != scene.palette) {
            palette_source_ = scene.palette;
            for (std::size_t index = 0U; index < scene.categories.size(); ++index)
                raster::detail::color::hsv_to_rgb(scene.palette[index].hue, scene.palette[index].saturation, scene.palette[index].value,
                                                palette_[index].r, palette_[index].g, palette_[index].b);
        }
        for (std::size_t index = 0; index < document_.RenderObjectCount(); ++index) {
            const auto& object = document_.RenderObjectAt(index);
            if (!object.enabled) continue;
            const auto color = palette_[object.category];
            if (!object.mask.runs.empty()) {
                if (raster::raster_mask_runs_rgba(
                        {.overlay = {reinterpret_cast<std::uint8_t*>(semantic.data), semantic.descriptor.pitch_bytes,
                                     static_cast<int>(semantic.descriptor.width), static_cast<int>(semantic.descriptor.height)},
                         .run_pairs = static_cast<const std::uint32_t*>(mask_device_.active()) + offset * 2U,
                         .run_count = static_cast<std::uint32_t>(object.mask.runs.size()),
                         .color = {color.r, color.g, color.b, 92U},
                         .stream = {reinterpret_cast<void*>(stream_value)}}) != 0)
                    throw std::runtime_error("Annotation mask rendering failed");
                offset += object.mask.runs.size();
            }
            const raster::MutableBytes overlay{reinterpret_cast<std::uint8_t*>(semantic.data), semantic.descriptor.pitch_bytes,
                                               static_cast<int>(semantic.descriptor.width), static_cast<int>(semantic.descriptor.height)};
            const raster::NativeStream native_stream{reinterpret_cast<void*>(stream_value)};
            const auto& geometry = geometry_[index];
            const auto* words = total_words ? static_cast<const std::uint32_t*>(mask_device_.active()) + run_count * 2 : nullptr;
            const raster::PointBuffer points{geometry.count ? reinterpret_cast<const int*>(words + geometry.offset) : nullptr,
                                             geometry.count};
            int draw_status = 0;
            if (object.shape == domain::AnnotationShape::Spline && geometry.count > 1)
                draw_status = raster::raster_polyline_rgba({overlay, points, object.spline_closed, color, 2, native_stream});
            if ((object.shape == domain::AnnotationShape::Point || object.shape == domain::AnnotationShape::Spline) && geometry.count == 1)
                draw_status = raster::raster_points_rgba({overlay, points, 4, {color.r, color.g, color.b, 255}, native_stream});
            if (object.shape == domain::AnnotationShape::Skeleton && geometry.edges)
                draw_status = raster::raster_skeleton_rgba(
                    {overlay, points, {words + geometry.offset + geometry.edge_offset, geometry.edges}, color, 2, native_stream});
            if (draw_status != 0) throw std::runtime_error("Annotation geometry rendering failed");
            if (geometry.handles &&
                raster::raster_points_rgba({overlay,
                                            {reinterpret_cast<const int*>(words + geometry.offset + geometry.handle_offset), geometry.handles},
                                            4,
                                            {color.r, color.g, color.b, 255},
                                            native_stream}) != 0)
                throw std::runtime_error("Annotation vertex rendering failed");
            if (object.shape != domain::AnnotationShape::Box && object.shape != domain::AnnotationShape::Mask) continue;
            if (document_.ui().editor.selected_object == index &&
                raster::raster_selection_handles_rgba(
                    {overlay,
                     {static_cast<int>(object.box.first.x) - 5, static_cast<int>(object.box.first.y) - 5,
                      static_cast<int>(object.box.second.x) + 5, static_cast<int>(object.box.second.y) + 5},
                     3,
                     {255, 255, 255, 255},
                     native_stream}) != 0)
                throw std::runtime_error("Annotation selection rendering failed");
            if (object.box.first.x >= object.box.second.x || object.box.first.y >= object.box.second.y) continue;
            if (raster::raster_box_outline_rgba({
                    .overlay =
                        {
                            reinterpret_cast<std::uint8_t*>(semantic.data),
                            semantic.descriptor.pitch_bytes,
                            static_cast<int>(semantic.descriptor.width),
                            static_cast<int>(semantic.descriptor.height),
                        },
                    .box =
                        {
                            static_cast<int>(object.box.first.x),
                            static_cast<int>(object.box.first.y),
                            static_cast<int>(object.box.second.x),
                            static_cast<int>(object.box.second.y),
                        },
                    .color = color,
                    .thickness = 2,
                    .stream = {reinterpret_cast<void*>(stream_value)},
                }) != 0)
                throw std::runtime_error("Annotation semantic rendering failed");
        }
    }

    document::AnnotationDocument document_;
    const std::exception_ptr release_failure_ = std::make_exception_ptr(std::runtime_error("Annotation mask release failed"));
    VisualRegion crop_{};
    // The runtime owns this input until the next Open or resource teardown.
    mmltk::frameworks::gpu::ImagePlaneView source_{};
    std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> sample_host_;
    struct Geometry {
        std::vector<std::uint32_t> words;
        std::uint64_t scene_revision = 0U;
        bool preview = false;
        std::size_t offset = 0, edge_offset = 0, handle_offset = 0;
        int count = 0, edges = 0, handles = 0;
    };
    mutable std::vector<domain::AnnotationColor> palette_source_;
    mutable std::array<raster::RgbColor, domain::kAnnotationCategoryCapacity> palette_{};
    mutable std::vector<Geometry> geometry_;
    mutable std::vector<std::uint32_t> geometry_words_;
    mutable mmltk::frameworks::gpu::CudaHighWaterAllocation<void*> mask_device_;
    mutable std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> mask_host_;
    mutable std::size_t mask_capacity_ = 0U;
    mutable cudaEvent_t mask_upload_ = nullptr;
    void EnsureMasks(const std::size_t bytes) const {
        if (bytes <= mask_capacity_) return;
        if (!mask_host_) mask_host_ = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
        mask_host_->ensure_bytes(bytes);
        if (!mask_device_.AllocateCandidate([bytes](void*& p) { return cudaMalloc(&p, bytes); }).released() ||
            !mask_device_.PromoteCandidate([](void* p) { return cudaFree(p); }).released())
            throw std::runtime_error("Annotation mask allocation failed");
        mask_capacity_ = bytes;
    }
};

}  // namespace

VisualRuntimeFactory make_native_annotation_runtime_factory(const VisualDeviceSettings settings) {
    if (!settings.valid()) throw contracts::InvalidIntentError("Annotation native configuration is invalid");
    return [settings, execution = resolve_visual_device_execution(settings)](auto revisions) {
        return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
            .device = settings.device,
            .model = std::make_unique<NativeAnnotationAlgorithm>(),
            .input_layout = mmltk::frameworks::gpu::ImageProductLayout::Clean,
            .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            .numa_node = settings.numa_node,
            .execution = execution,
            .product_revisions = std::move(revisions),
        });
    };
}

}  // namespace mmltk::controller
