#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/annotation/detail/annotation_render_state.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include <cuda_runtime_api.h>
#include <algorithm>
#include <array>
#include <limits>
#include <cmath>
#include <vector>
#include <stdexcept>
#include <utility>
#include <iterator>
#include <optional>
#include "src/frameworks/gpu/cuda_high_water_allocation.h"
#include "src/frameworks/gpu/cuda_error.h"
#include "src/frameworks/gpu/pinned_host_buffer.h"
#include "src/backend/imaging/raster/class_palette.h"
import mmltk.backend.imaging.raster;
namespace mmltk::controller {
namespace {
namespace raster = mmltk::backend::imaging::raster;
namespace domain = mmltk::controller::contracts;
}  // namespace
namespace {
class NativeAnnotationAlgorithm final : public AnnotationAlgorithm {
   public:
    [[nodiscard]] mmltk::frameworks::gpu::ImageWorkspaceCoverage WorkspaceCoverage(
        const mmltk::frameworks::gpu::ImageWorkspaceObservation& baseline) const override {
        return {.allocation_identity = baseline.workspace ? baseline.workspace->identity() : 0U,
                .regions = {&damage_, 1U},
                .full_image = full_damage_,
                .baseline = {baseline.product_owner, baseline.product_revision}};
    }
    void Open(const mmltk::frameworks::gpu::ImagePlaneView source, const VisualRegion crop, const VisualExtent target) override {
        crop_ = crop.valid() ? crop : VisualRegion{0U, 0U, source.descriptor.width, source.descriptor.height};
        target_ = target;
        source_ = source;
        allocations_.clear();
        geometry_.clear();
        upload_valid_ = upload_pending_ = false;
    }
    Release ReleaseResources() noexcept override {
        if (mask_upload_ != nullptr) {
            if (cudaEventDestroy(mask_upload_) != cudaSuccess) return {.all_released = false, .failure = release_failure_};
            mask_upload_ = nullptr;
        }
        const auto device = mask_device_.ReleaseAll([](void* p) { return cudaFree(p); });
        const auto sample = sample_host_ ? sample_host_->ReleaseSettled() : CUDA_SUCCESS;
        const auto host = mask_host_ ? mask_host_->ReleaseSettled() : CUDA_SUCCESS;
        if (!device.released() || host != CUDA_SUCCESS || sample != CUDA_SUCCESS) return {.all_released = false, .failure = release_failure_};
        return {};
    }
    [[nodiscard]] domain::AnnotationColor Sample(const domain::AnnotationPoint point) override {
        if (!sample_host_) sample_host_ = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
        sample_host_->ensure_bytes(4);
        const auto x =
            std::min(static_cast<unsigned>(point.x * static_cast<float>(crop_.width) / static_cast<float>(target_.width)), crop_.width - 1U) + crop_.x;
        const auto y =
            std::min(static_cast<unsigned>(point.y * static_cast<float>(crop_.height) / static_cast<float>(target_.height)), crop_.height - 1U) + crop_.y;
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
        return {hue, high == 0 ? 0 : delta / high, high};
    }

   private:
    struct Footprint final {
        raster::IntRect bounds{};
        raster::IntRect runs{};
    };
    struct Allocation final {
        std::uint64_t owner = 0U, identity = 0U, semantic = 0U, epoch = 0U;
        std::uint32_t width = 0U, height = 0U;
        std::optional<std::uint16_t> selected{};
        std::vector<domain::AnnotationObject> objects{};
        std::vector<Footprint> footprints{};
        std::vector<std::optional<AnnotationDragPreview>> transforms{};
        std::vector<domain::AnnotationColor> palette{};
    };
    mutable std::vector<Allocation> allocations_;
    mutable std::vector<Footprint> footprints_;
    mutable mmltk::frameworks::gpu::ImageWorkspaceRegion damage_;
    mutable bool full_damage_ = true;
    static bool SameGeometry(const domain::AnnotationObject& a, const domain::AnnotationObject& b) {
        return a.shape == b.shape && a.point == b.point && a.spline_knots == b.spline_knots && a.skeleton_nodes == b.skeleton_nodes &&
               a.skeleton_edges == b.skeleton_edges && a.spline_closed == b.spline_closed;
    }
    static bool SameDrawing(const domain::AnnotationObject& a, const domain::AnnotationObject& b, bool same_runs) {
        return SameGeometry(a, b) && a.box == b.box && same_runs && a.category == b.category && a.enabled == b.enabled;
    }
    static domain::AnnotationBox DrawingBox(const AnnotationRenderState& description, std::size_t index) {
        const auto& object = description.DrawingObjectAt(index);
        if (!description.drag || description.preview_object != index ||
            (object.shape != domain::AnnotationShape::Box && object.shape != domain::AnnotationShape::Mask))
            return object.box;
        const auto box = description.TargetBox(index);
        if (!description.TransformsMask(index)) return box;
        if (box.first.x >= box.second.x || box.first.y >= box.second.y) return {};
        return {{std::floor(box.first.x), std::floor(box.first.y)}, {std::ceil(box.second.x), std::ceil(box.second.y)}};
    }
    static raster::IntRect RunBounds(const domain::AnnotationObject& object) {
        if (object.mask.runs.empty()) return {};
        raster::IntRect bounds{std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), 0, 0};
        for (const auto run : object.mask.runs) {
            bounds.x1 = std::min(bounds.x1, static_cast<int>(run.first));
            bounds.y1 = std::min(bounds.y1, static_cast<int>(run.row));
            bounds.x2 = std::max(bounds.x2, static_cast<int>(run.last) + 1);
            bounds.y2 = std::max(bounds.y2, static_cast<int>(run.row) + 1);
        }
        return bounds;
    }
    template <class Visit>
    static void VisitSplineHandles(const AnnotationRenderState& description, std::size_t index, Visit visit) {
        const auto& knots = description.DrawingObjectAt(index).spline_knots;
        for (std::size_t item = 0; item < knots.size(); ++item) {
            const auto knot = description.DrawingKnot(index, item);
            visit(knot.point);
            if (knot.in.enabled) visit(knot.in.point);
            if (knot.out.enabled) visit(knot.out.point);
        }
    }
    static raster::IntRect Bounds(const AnnotationRenderState& description, std::size_t index, raster::IntRect runs, int width, int height) {
        const auto& object = description.DrawingObjectAt(index);
        if (!object.enabled) return {};
        const auto box = DrawingBox(description, index);
        float x1 = box.first.x, y1 = box.first.y, x2 = box.second.x, y2 = box.second.y;
        if (object.shape != domain::AnnotationShape::Box && object.shape != domain::AnnotationShape::Mask)
            x1 = x2 = description.DrawingPoint(index).x, y1 = y2 = description.DrawingPoint(index).y;
        const auto point = [&](domain::AnnotationPoint value) {
            x1 = std::min(x1, value.x);
            y1 = std::min(y1, value.y);
            x2 = std::max(x2, value.x);
            y2 = std::max(y2, value.y);
        };
        VisitSplineHandles(description, index, point);
        for (std::size_t node = 0; node < object.skeleton_nodes.size(); ++node)
            if (object.skeleton_nodes[node].visible) point(description.DrawingNode(index, node));
        // Geometry includes the existing outline thickness and selection/vertex
        // handles. Runs are independent drawing primitives for every named shape.
        x1 = std::floor(x1) - 9;
        y1 = std::floor(y1) - 9;
        x2 = std::ceil(x2) + 9;
        y2 = std::ceil(y2) + 9;
        if (runs.x1 < runs.x2 && runs.y1 < runs.y2) {
            float first = static_cast<float>(runs.x1), top = static_cast<float>(runs.y1);
            float last = static_cast<float>(runs.x2), bottom = static_cast<float>(runs.y2);
            bool visible = true;
            if (description.TransformsMask(index)) {
                const auto target = description.TargetBox(index);
                const float sx =
                    object.box.second.x > object.box.first.x ? (target.second.x - target.first.x) / (object.box.second.x - object.box.first.x) : 1.0F;
                const float sy =
                    object.box.second.y > object.box.first.y ? (target.second.y - target.first.y) / (object.box.second.y - object.box.first.y) : 1.0F;
                visible = sx > 0 && sy > 0;
                const auto project = [](float value, float source, float target_coordinate, float scale) {
                    // Keep the same separate float operations as the raster kernel.
                    const volatile float delta = value - source;
                    const volatile float scaled = delta * scale;
                    return target_coordinate + scaled;
                };
                first = std::floor(project(first, object.box.first.x, target.first.x, sx));
                last = std::ceil(project(last, object.box.first.x, target.first.x, sx));
                top = std::floor(project(top, object.box.first.y, target.first.y, sy));
                bottom = std::ceil(project(bottom, object.box.first.y, target.first.y, sy));
            }
            if (visible) {
                x1 = std::min(x1, first);
                y1 = std::min(y1, top);
                x2 = std::max(x2, last);
                y2 = std::max(y2, bottom);
            }
        }
        return {static_cast<int>(std::clamp(x1, 0.0F, static_cast<float>(width))), static_cast<int>(std::clamp(y1, 0.0F, static_cast<float>(height))),
                static_cast<int>(std::clamp(x2, 0.0F, static_cast<float>(width))), static_cast<int>(std::clamp(y2, 0.0F, static_cast<float>(height)))};
    }
    void Render(const AnnotationRenderState& description, const mmltk::frameworks::gpu::ImagePlaneView source,
                const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                const std::uintptr_t stream_value) const override {
        auto stream = reinterpret_cast<cudaStream_t>(stream_value);
        auto allocation = std::ranges::find(allocations_, clean.allocation.owner, &Allocation::owner);
        if (allocation == allocations_.end()) {
            allocations_.push_back({.owner = clean.allocation.owner});
            allocation = std::prev(allocations_.end());
        }
        auto& retained = *allocation;
        const bool initialize = retained.identity != clean.allocation.identity || retained.epoch != description.document_epoch ||
                                retained.width != clean.descriptor.width || retained.height != clean.descriptor.height ||
                                retained.semantic != semantic.allocation.identity;
        if (initialize) {
            const raster::ConstBytes input{reinterpret_cast<const std::uint8_t*>(source.data + crop_.y * source.descriptor.pitch_bytes + crop_.x * 4U),
                                           source.descriptor.pitch_bytes, static_cast<int>(crop_.width), static_cast<int>(crop_.height)};
            const raster::MutableBytes output{reinterpret_cast<std::uint8_t*>(clean.data), clean.descriptor.pitch_bytes,
                                              static_cast<int>(clean.descriptor.width), static_cast<int>(clean.descriptor.height)};
            if (!source.valid()) throw std::runtime_error("Annotation clean baseline source is unavailable");
            const auto status = input.width == output.width && input.height == output.height
                                    ? cudaMemcpy2DAsync(output.pixels, output.pitch_bytes, input.pixels, input.pitch_bytes, clean.descriptor.row_bytes(),
                                                        clean.descriptor.height, cudaMemcpyDeviceToDevice, stream)
                                    : static_cast<cudaError_t>(raster::scale_rgba_nearest(input, output, stream_value));
            if (status != cudaSuccess) throw std::runtime_error("Annotation clean baseline preparation failed");
        }
        raster::IntRect clip{static_cast<int>(clean.descriptor.width), static_cast<int>(clean.descriptor.height), 0, 0};
        const auto damage = [&](raster::IntRect bounds) {
            if (bounds.x1 >= bounds.x2 || bounds.y1 >= bounds.y2) return;
            // CLEANUP-IGNORE: Raster clipping expands an image-sized sentinel; workspace damage separately adopts its first valid region
            // and has a different coordinate type.
            clip.x1 = std::min(clip.x1, bounds.x1);
            clip.y1 = std::min(clip.y1, bounds.y1);
            clip.x2 = std::max(clip.x2, bounds.x2);
            clip.y2 = std::max(clip.y2, bounds.y2);
        };
        const bool palette_changed = retained.palette != description.scene->palette;
        footprints_.resize(description.ObjectCount());
        for (std::size_t index = 0; index < std::max(retained.objects.size(), description.ObjectCount()); ++index) {
            const bool previous = index < retained.objects.size();
            const bool current = index < description.ObjectCount();
            const bool same_runs = previous && current && retained.objects[index].mask.runs == description.DrawingObjectAt(index).mask.runs;
            if (current) {
                const auto& object = description.DrawingObjectAt(index);
                auto& footprint = footprints_[index];
                footprint.runs = same_runs ? retained.footprints[index].runs : RunBounds(object);
                footprint.bounds =
                    Bounds(description, index, footprint.runs, static_cast<int>(clean.descriptor.width), static_cast<int>(clean.descriptor.height));
            }
            const auto bounds = current ? footprints_[index].bounds : raster::IntRect{};
            const bool changed = !previous || !current || palette_changed || (retained.selected == index) != (description.editor.selected_object == index) ||
                                 !SameDrawing(retained.objects[index], description.DrawingObjectAt(index), same_runs) ||
                                 retained.footprints[index].bounds.x1 != bounds.x1 || retained.footprints[index].bounds.y1 != bounds.y1 ||
                                 retained.footprints[index].bounds.x2 != bounds.x2 || retained.footprints[index].bounds.y2 != bounds.y2 ||
                                 retained.transforms[index] != (description.preview_object == index ? description.drag : std::nullopt);
            if (changed) {
                if (previous) damage(retained.footprints[index].bounds);
                if (current) damage(bounds);
            }
        }
        if (initialize) clip = {0, 0, static_cast<int>(clean.descriptor.width), static_cast<int>(clean.descriptor.height)};
        clip.x1 = std::max(0, clip.x1);
        clip.y1 = std::max(0, clip.y1);
        clip.x2 = std::min(static_cast<int>(clean.descriptor.width), clip.x2);
        clip.y2 = std::min(static_cast<int>(clean.descriptor.height), clip.y2);
        damage_ = {clip.x1, clip.y1, clip.x2, clip.y2};
        full_damage_ = initialize;
        if (clip.x1 >= clip.x2 || clip.y1 >= clip.y2) return;
        if (cudaMemset2DAsync(reinterpret_cast<void*>(semantic.data + clip.y1 * semantic.descriptor.pitch_bytes + clip.x1 * 4U),
                              semantic.descriptor.pitch_bytes, 0, static_cast<std::size_t>(clip.x2 - clip.x1) * 4U, static_cast<std::size_t>(clip.y2 - clip.y1),
                              stream) != cudaSuccess)
            throw std::runtime_error("Annotation semantic damage preparation failed");
        const auto& scene = *description.scene;
        // Reuse pinned staging only after its prior transfer has consumed it;
        // the rendering kernels themselves remain ordered on the runtime stream.
        if (mask_upload_ != nullptr && cudaEventSynchronize(mask_upload_) != cudaSuccess) {
            upload_valid_ = upload_pending_ = false;
            throw std::runtime_error("Annotation mask upload settlement failed");
        }
        if (upload_pending_) {
            upload_valid_ = true;
            upload_pending_ = false;
        }
        std::size_t run_count = 0U, total_words = 0U;
        try {
            for (std::size_t index = 0; index < description.ObjectCount(); ++index) {
                const auto& object = description.DrawingObjectAt(index);
                if (object.enabled) run_count += object.mask.runs.size();
            }
            if (geometry_.size() < description.ObjectCount()) geometry_.resize(description.ObjectCount());
            // Absent indices retain capacity, but no longer own words in the
            // compacted layout. Restoration must refill both runs and geometry.
            for (std::size_t index = description.ObjectCount(); index < geometry_.size(); ++index) geometry_[index].enabled = false;
            std::size_t geometry_word_count = 0U;
            for (std::size_t index = 0; index < description.ObjectCount(); ++index) {
                const auto& object = description.DrawingObjectAt(index);
                if (!object.enabled) continue;
                auto& geometry = geometry_[index];
                const auto transform = description.preview_object == index ? description.drag : std::nullopt;
                geometry.changed = !geometry.source || !SameGeometry(*geometry.source, object) || geometry.transform != transform;
                if (geometry.changed) {
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
                    if (object.shape == domain::AnnotationShape::Point) point(description.DrawingPoint(index));
                    if (object.shape == domain::AnnotationShape::Spline && !object.spline_knots.empty()) {
                        point(description.DrawingKnot(index, 0U).point);
                        const auto segments = object.spline_knots.size() - (object.spline_closed ? 0U : 1U);
                        for (std::size_t segment = 0; segment < segments; ++segment) {
                            const auto a = description.DrawingKnot(index, segment);
                            const auto b = description.DrawingKnot(index, (segment + 1) % object.spline_knots.size());
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
                        for (std::size_t node = 0; node < object.skeleton_nodes.size(); ++node) point(description.DrawingNode(index, node));
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
                        for (std::size_t node = 0; node < object.skeleton_nodes.size(); ++node)
                            if (object.skeleton_nodes[node].visible) handle(description.DrawingNode(index, node));
                    if (object.shape == domain::AnnotationShape::Spline) VisitSplineHandles(description, index, handle);
                    if (!geometry.source) geometry.source.emplace();
                    geometry.source->shape = object.shape;
                    geometry.source->point = object.point;
                    geometry.source->spline_knots = object.spline_knots;
                    geometry.source->skeleton_nodes = object.skeleton_nodes;
                    geometry.source->skeleton_edges = object.skeleton_edges;
                    geometry.source->spline_closed = object.spline_closed;
                    geometry.transform = transform;
                }
                geometry.next_offset = geometry_word_count;
                geometry_word_count += geometry.words.size();
            }
            total_words = run_count * 2 + geometry_word_count;
            const bool reusable = upload_valid_ && upload_width_ == clean.descriptor.width;
            // Invalidate before touching staging, allocation, or the authoritative word
            // mirror. Successfully recorded pending bytes are ordered for this draw;
            // only the existing upload settlement above makes them reusable later.
            upload_valid_ = false;
            const bool grew = total_words * sizeof(std::uint32_t) > mask_capacity_;
            EnsureMasks(total_words * sizeof(std::uint32_t));
            uploaded_words_.resize(total_words);
            std::size_t first = total_words, last = 0U, run_offset = 0U;
            const auto changed = [&](std::size_t begin, std::size_t end) {
                if (begin == end) return;
                first = std::min(first, begin);
                last = std::max(last, end);
            };
            for (std::size_t index = 0; index < description.ObjectCount(); ++index) {
                const auto& object = description.DrawingObjectAt(index);
                auto& geometry = geometry_[index];
                if (!object.enabled) {
                    geometry.enabled = false;
                    continue;
                }
                if (!reusable || !geometry.enabled || geometry.run_offset != run_offset || geometry.runs != object.mask.runs) {
                    auto word = run_offset * 2U;
                    for (const auto run : object.mask.runs) {
                        uploaded_words_[word++] = static_cast<std::uint32_t>(run.row) * clean.descriptor.width + run.first;
                        uploaded_words_[word++] = static_cast<std::uint32_t>(run.last) - run.first + 1U;
                    }
                    changed(run_offset * 2U, word);
                    if (geometry.runs != object.mask.runs) geometry.runs = object.mask.runs;
                }
                const auto word_offset = run_count * 2U + geometry.next_offset;
                if (!reusable || !geometry.enabled || geometry.changed || geometry.word_offset != word_offset) {
                    std::copy(geometry.words.begin(), geometry.words.end(), uploaded_words_.begin() + word_offset);
                    changed(word_offset, word_offset + geometry.words.size());
                }
                geometry.offset = geometry.next_offset;
                geometry.word_offset = word_offset;
                geometry.run_offset = run_offset;
                geometry.enabled = true;
                run_offset += object.mask.runs.size();
            }
            upload_width_ = clean.descriptor.width;
            if (grew || !reusable) {
                first = 0U;
                last = total_words;
            }
            if (last > first) {
                auto* const pairs = static_cast<std::uint32_t*>(mask_host_->data());
                std::copy(uploaded_words_.begin() + first, uploaded_words_.begin() + last, pairs + first);
                if (cudaMemcpyAsync(static_cast<std::uint32_t*>(mask_device_.active()) + first, pairs + first, (last - first) * sizeof(std::uint32_t),
                                    cudaMemcpyHostToDevice, stream) != cudaSuccess)
                    throw std::runtime_error("Annotation changed geometry upload failed");
                if (mask_upload_ == nullptr && cudaEventCreateWithFlags(&mask_upload_, cudaEventDisableTiming) != cudaSuccess)
                    throw std::runtime_error("Annotation mask upload event creation failed");
                if (cudaEventRecord(mask_upload_, stream) != cudaSuccess) throw std::runtime_error("Annotation mask upload event recording failed");
                upload_pending_ = true;
            } else {
                upload_valid_ = true;
            }
        } catch (...) {
            upload_valid_ = upload_pending_ = false;
            for (auto& geometry : geometry_) geometry.source.reset();
            throw;
        }
        std::size_t offset = 0U;
        if (palette_source_ != scene.palette) {
            palette_source_ = scene.palette;
            for (std::size_t index = 0U; index < scene.categories.size(); ++index)
                raster::color::hsv_to_rgb(scene.palette[index].hue, scene.palette[index].saturation, scene.palette[index].value, palette_[index].r,
                                          palette_[index].g, palette_[index].b);
        }
        // CLEANUP-IGNORE: Enabled-object traversal feeds distinct geometry, RLE packing, and drawing bodies; no repeated loop body exists.
        for (std::size_t index = 0; index < description.ObjectCount(); ++index) {
            const auto& object = description.DrawingObjectAt(index);
            if (!object.enabled) continue;
            const auto bounds = footprints_[index].bounds;
            if (bounds.x1 >= clip.x2 || bounds.x2 <= clip.x1 || bounds.y1 >= clip.y2 || bounds.y2 <= clip.y1) {
                offset += object.mask.runs.size();
                continue;
            }
            const raster::IntRect object_clip{std::max(clip.x1, bounds.x1), std::max(clip.y1, bounds.y1), std::min(clip.x2, bounds.x2),
                                              std::min(clip.y2, bounds.y2)};
            const auto box = DrawingBox(description, index);
            const auto target = description.TransformsMask(index) ? description.TargetBox(index) : object.box;
            const auto color = palette_[object.category];
            if (!object.mask.runs.empty()) {
                if (raster::raster_mask_runs_rgba(
                        {.overlay = {reinterpret_cast<std::uint8_t*>(semantic.data), semantic.descriptor.pitch_bytes,
                                     static_cast<int>(semantic.descriptor.width), static_cast<int>(semantic.descriptor.height)},
                         .run_pairs = static_cast<const std::uint32_t*>(mask_device_.active()) + offset * 2U,
                         .run_count = static_cast<std::uint32_t>(object.mask.runs.size()),
                         .color = {color.r, color.g, color.b, 92U},
                         .stream = {reinterpret_cast<void*>(stream_value)},
                         .clip = object_clip,
                         .source_x = object.box.first.x,
                         .source_y = object.box.first.y,
                         .target_x = target.first.x,
                         .target_y = target.first.y,
                         .scale_x =
                             object.box.second.x > object.box.first.x ? (target.second.x - target.first.x) / (object.box.second.x - object.box.first.x) : 1.0F,
                         .scale_y = object.box.second.y > object.box.first.y ? (target.second.y - target.first.y) / (object.box.second.y - object.box.first.y)
                                                                             : 1.0F}) != 0)
                    throw std::runtime_error("Annotation mask rendering failed");
                offset += object.mask.runs.size();
            }
            const raster::MutableBytes overlay{reinterpret_cast<std::uint8_t*>(semantic.data), semantic.descriptor.pitch_bytes,
                                               static_cast<int>(semantic.descriptor.width), static_cast<int>(semantic.descriptor.height)};
            const raster::NativeStream native_stream{reinterpret_cast<void*>(stream_value)};
            const auto& geometry = geometry_[index];
            const auto* words = total_words ? static_cast<const std::uint32_t*>(mask_device_.active()) + run_count * 2 : nullptr;
            const raster::PointBuffer points{geometry.count ? reinterpret_cast<const int*>(words + geometry.offset) : nullptr, geometry.count};
            int draw_status = 0;
            if (object.shape == domain::AnnotationShape::Spline && geometry.count > 1)
                draw_status = raster::raster_polyline_rgba({overlay, points, object.spline_closed, color, 2, native_stream, object_clip});
            if ((object.shape == domain::AnnotationShape::Point || object.shape == domain::AnnotationShape::Spline) && geometry.count == 1)
                draw_status = raster::raster_points_rgba({overlay, points, 4, {color.r, color.g, color.b, 255}, native_stream, object_clip});
            if (object.shape == domain::AnnotationShape::Skeleton && geometry.edges)
                draw_status = raster::raster_skeleton_rgba(
                    {overlay, points, {words + geometry.offset + geometry.edge_offset, geometry.edges}, color, 2, native_stream, object_clip});
            if (draw_status != 0) throw std::runtime_error("Annotation geometry rendering failed");
            if (geometry.handles && (object.shape != domain::AnnotationShape::Spline || description.editor.selected_object == index) &&
                raster::raster_points_rgba({overlay,
                                            {reinterpret_cast<const int*>(words + geometry.offset + geometry.handle_offset), geometry.handles},
                                            4,
                                            {color.r, color.g, color.b, 255},
                                            native_stream,
                                            object_clip}) != 0)
                throw std::runtime_error("Annotation vertex rendering failed");
            if (object.shape != domain::AnnotationShape::Box && object.shape != domain::AnnotationShape::Mask) continue;
            if (description.editor.selected_object == index &&
                raster::raster_selection_handles_rgba({overlay,
                                                       {static_cast<int>(box.first.x) - 5, static_cast<int>(box.first.y) - 5,
                                                        static_cast<int>(box.second.x) + 5, static_cast<int>(box.second.y) + 5},
                                                       3,
                                                       {255, 255, 255, 255},
                                                       native_stream,
                                                       object_clip}) != 0)
                throw std::runtime_error("Annotation selection rendering failed");
            const raster::IntRect outline{static_cast<int>(box.first.x), static_cast<int>(box.first.y), static_cast<int>(box.second.x),
                                          static_cast<int>(box.second.y)};
            if (outline.x1 >= outline.x2 || outline.y1 >= outline.y2) continue;
            mmltk::frameworks::gpu::ensure_cuda_ok(
                static_cast<cudaError_t>(raster::raster_box_outline_rgba(
                    {.overlay = overlay, .box = outline, .color = color, .thickness = 2, .stream = native_stream, .clip = object_clip})),
                "Annotation semantic rendering failed");
        }
        retained.epoch = description.document_epoch;
        retained.identity = clean.allocation.identity;
        retained.width = clean.descriptor.width;
        retained.height = clean.descriptor.height;
        retained.semantic = semantic.allocation.identity;
        retained.selected = description.editor.selected_object;
        retained.palette = description.scene->palette;
        retained.objects.resize(description.ObjectCount());
        retained.footprints = footprints_;
        retained.transforms.resize(description.ObjectCount());
        for (std::size_t index = 0; index < description.ObjectCount(); ++index) {
            const auto& object = description.DrawingObjectAt(index);
            if (retained.objects[index] != object) retained.objects[index] = object;
            retained.transforms[index] = description.preview_object == index ? description.drag : std::nullopt;
        }
    }
    const std::exception_ptr release_failure_ = std::make_exception_ptr(std::runtime_error("Annotation mask release failed"));
    VisualRegion crop_{};
    VisualExtent target_{};
    // The runtime owns this input until the next Open or resource teardown.
    mmltk::frameworks::gpu::ImagePlaneView source_{};
    std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> sample_host_;
    struct Geometry {
        std::vector<std::uint32_t> words;
        std::optional<domain::AnnotationObject> source;
        std::optional<AnnotationDragPreview> transform;
        std::vector<domain::AnnotationMaskRun> runs;
        std::size_t run_offset = 0U, word_offset = 0U, next_offset = 0U;
        bool enabled = false, changed = false;
        std::size_t offset = 0, edge_offset = 0, handle_offset = 0;
        int count = 0, edges = 0, handles = 0;
    };
    mutable std::vector<domain::AnnotationColor> palette_source_;
    mutable std::array<raster::RgbColor, domain::kAnnotationCategoryCapacity> palette_{};
    mutable std::vector<Geometry> geometry_;
    mutable std::vector<std::uint32_t> uploaded_words_;
    mutable mmltk::frameworks::gpu::CudaHighWaterAllocation<void*> mask_device_;
    mutable std::unique_ptr<mmltk::frameworks::gpu::PinnedHostBuffer> mask_host_;
    mutable std::size_t mask_capacity_ = 0U;
    mutable cudaEvent_t mask_upload_ = nullptr;
    mutable bool upload_valid_ = false, upload_pending_ = false;
    mutable std::uint32_t upload_width_ = 0U;
    void EnsureMasks(const std::size_t bytes) const {
        if (bytes <= mask_capacity_) return;
        const auto capacity =
            mask_capacity_ <= std::numeric_limits<std::size_t>::max() - mask_capacity_ / 2U ? std::max(bytes, mask_capacity_ + mask_capacity_ / 2U) : bytes;
        if (!mask_host_) mask_host_ = mmltk::frameworks::gpu::PinnedHostBuffer::ForCurrentDevice();
        mask_host_->ensure_bytes(capacity);
        if (!mask_device_.AllocateCandidate([capacity](void*& p) { return cudaMalloc(&p, capacity); }).released() ||
            !mask_device_.PromoteCandidate([](void* p) { return cudaFree(p); }).released())
            throw std::runtime_error("Annotation mask allocation failed");
        mask_capacity_ = capacity;
    }
};
}  // namespace
VisualRuntimeFactory make_native_annotation_runtime_factory(const VisualDeviceSettings settings) {
    if (!settings.valid()) throw contracts::InvalidIntentError("Annotation native configuration is invalid");
    return [settings, execution = resolve_visual_device_execution(settings)](auto revisions) {
        mmltk::frameworks::gpu::SystemImageRuntimeConfig config{
            .device = settings.device,
            .model = std::make_unique<NativeAnnotationAlgorithm>(),
            .input_layout = mmltk::frameworks::gpu::ImageProductLayout::Clean,
            .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
            .output_buffer_count = 2U,
            .numa_node = settings.numa_node,
            .execution = execution,
            .product_revisions = std::move(revisions),
        };
        configure_visual_workspace_finalization(config);
        return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(std::move(config));
    };
}
}  // namespace mmltk::controller
