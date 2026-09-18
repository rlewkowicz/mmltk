#include "src/controller/subsystems/explore/detail/gallery_stream_probe.h"
#include "src/controller/subsystems/explore/detail/gallery_descriptor_storage.h"
#include "src/controller/subsystems/explore/detail/gallery_stream.h"
#include "src/controller/subsystems/explore/detail/gallery_payload.h"
#include "src/frameworks/gpu/image_failure.h"
#include <algorithm>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <exception>
#include <utility>
import mmltk.backend.imaging.explore.explore_render_core;
namespace mmltk::controller::explore_detail {
namespace explore = mmltk::backend::imaging::explore;
GalleryStreamProbe::GalleryStreamProbe(int device, VisualDiagnosticSink diagnostics, std::shared_ptr<ExploreAcceptanceGate> acceptance)
    : device_(device), diagnostics_(diagnostics), acceptance_(std::move(acceptance)) {
    storage_.Bind(host_allocations_.api());
}
void GalleryStreamProbe::DiagnoseRendered(const GalleryProductState& product, const GalleryDescriptorStorage& descriptors,
                                          const mmltk::frameworks::gpu::ImagePlaneView reference_plane, const GalleryThumbnailCache* incumbent,
                                          const mmltk::frameworks::gpu::ImagePlaneView clean, const mmltk::frameworks::gpu::ImagePlaneView semantic,
                                          const std::uintptr_t stream, const std::uint64_t generation, const std::uint64_t slot,
                                          const std::uint32_t compiled_index, const std::optional<std::size_t> card_index, const std::uint32_t x,
                                          const std::uint32_t y, const std::uint32_t width, const std::uint32_t height) try {
    if (!diagnostics_.pixel_probes_enabled() || probes_disabled_) return;
    if (acceptance_) acceptance_->CheckProbe();
    std::optional<explore::ExploreRenderCardDescriptor> card;
    if (card_index)
        card = load_payload<explore::ExploreRenderCardDescriptor>(
            descriptors.storage_.buffers_.descriptors_.data(),
            descriptors.descriptor_layout_.cards.offset + *card_index * sizeof(explore::ExploreRenderCardDescriptor));
    const auto* sample_card = card ? &*card : nullptr;
    if (!sample_card && width != 0U && product.plan.mode == ExploreMode::Gallery && slot < product.tile_meanings.size()) {
        const auto& meaning = product.tile_meanings[slot];
        if (meaning && meaning->card.compiled_index == compiled_index) sample_card = &meaning->card;
    }
    std::size_t selected_annotations = 0U;
    std::size_t hidden_annotations = 0U;
    std::size_t selected_rle = 0U;
    std::uint16_t selected_class_identity = 0U;
    std::uint16_t hidden_class_identity = 0U;
    std::optional<explore::ExploreRenderAnnotationDescriptor> probe_annotation;
    for (std::size_t local = 0U; card && local != card->annotation_count; ++local) {
        const auto annotation = load_payload<explore::ExploreRenderAnnotationDescriptor>(
            descriptors.storage_.buffers_.descriptors_.data(),
            descriptors.descriptor_layout_.annotations.offset + (card->annotation_offset + local) * sizeof(explore::ExploreRenderAnnotationDescriptor));
        const bool selected = annotation.class_id < product.active_classes.size() && product.active_classes[annotation.class_id].visible != 0U;
        if (selected) {
            ++selected_annotations;
            selected_rle += annotation.rle_count;
            if (selected_class_identity == 0U) selected_class_identity = static_cast<std::uint16_t>(annotation.class_id + 1U);
            if (!probe_annotation && annotation.rle_count != 0U) probe_annotation = annotation;
        } else {
            ++hidden_annotations;
            if (hidden_class_identity == 0U) hidden_class_identity = static_cast<std::uint16_t>(annotation.class_id + 1U);
        }
    }
    if (card) {
        diagnostics_.Emit([&] {
            auto fact = Diagnostic(VisualDiagnosticOperation::ExploreOverlaySelectionProbe, generation);
            fact.value = selected_annotations;
            fact.detail = compiled_index;
            fact.context.capacity_width = static_cast<std::uint32_t>(hidden_annotations);
            fact.context.capacity_height = static_cast<std::uint32_t>(selected_rle);
            fact.context.staging_bytes = (slot << 32U) | (static_cast<std::uint64_t>(selected_class_identity) << 16U) | hidden_class_identity;
            return fact;
        });
    }
    if (probes_pending_.load(std::memory_order_acquire) || probe_count_ == probes_.size() || !InitializeProbes()) return;
    ensure_gallery_buffer(storage_.buffers_.semantic_count_device_, probes_.size() * kProbeFacts * sizeof(std::uint64_t),
                          "Explore rendered diagnostic device allocation failed", diagnostics_, device_, product.plan.generation);
    ensure_gallery_buffer(storage_.buffers_.semantic_count_pinned_, probes_.size() * kProbeFacts * sizeof(std::uint64_t),
                          "Explore rendered diagnostic staging allocation failed", diagnostics_, device_, product.plan.generation);
    auto* const cuda_stream = reinterpret_cast<cudaStream_t>(stream);
    auto count_target = gallery_target(semantic);
    auto checksum_target = gallery_target(clean);
    if (width != 0U || height != 0U) {
        if (width == 0U || height == 0U || x >= count_target.width || y >= count_target.height || width > count_target.width - x ||
            height > count_target.height - y)
            throw std::logic_error("Explore rendered diagnostic region is invalid");
        count_target.data += static_cast<std::size_t>(y) * count_target.pitch_bytes + static_cast<std::size_t>(x) * 4U;
        checksum_target.data += static_cast<std::size_t>(y) * checksum_target.pitch_bytes + static_cast<std::size_t>(x) * 4U;
        count_target.width = width;
        count_target.height = height;
        checksum_target.width = width;
        checksum_target.height = height;
    }
    auto* const device_facts = static_cast<std::uint64_t*>(storage_.buffers_.semantic_count_device_.data()) + probe_count_ * kProbeFacts;
    ensure_gallery_cuda(cudaMemsetAsync(device_facts, 0, kProbeFacts * sizeof(std::uint64_t), cuda_stream), "Explore rendered diagnostic clear failed");
    if (explore::count_explore_nonzero_alpha(count_target, device_facts, stream) != explore::kExploreStorageSuccess)
        throw std::runtime_error("Explore semantic diagnostic count failed");
    if (explore::checksum_explore_pixels(checksum_target, device_facts + 1U, stream) != explore::kExploreStorageSuccess)
        throw std::runtime_error("Explore image diagnostic checksum failed");
    const auto letterbox = product.store->geometry(compiled_index);
    const auto scale_coordinate = [](const std::uint32_t coordinate, const std::uint32_t destination, const std::uint32_t source) {
        return static_cast<std::uint32_t>((static_cast<std::uint64_t>(coordinate) * destination) / source);
    };
    const auto scale_interval = [&scale_coordinate](const std::uint32_t offset, const std::uint32_t extent, const std::uint32_t destination,
                                                    const std::uint32_t source) {
        const auto begin = scale_coordinate(offset, destination, source);
        const auto end = scale_coordinate(offset + extent, destination, source);
        return std::pair{begin, end - begin};
    };
    const auto [content_x, content_width] =
        card ? scale_interval(letterbox.offset_x, letterbox.resized_width, card->image_width, card->source_width) : std::pair{0U, 0U};
    const auto [content_y, content_height] =
        card ? scale_interval(letterbox.offset_y, letterbox.resized_height, card->image_height, card->source_height) : std::pair{0U, 0U};
    explore::ExploreRenderTargetView reference{};
    if (sample_card && product.cache.size() != 0U) {
        reference = gallery_target(reference_plane);
        const auto position = static_cast<std::size_t>(product.viewport.first_row) * product.viewport.columns + slot;
        const auto* retained = product.cache.Retained(compiled_index);
        const auto bank = retained ? retained->bank : product.cache.WritableBank(position, incumbent);
        reference.data += product.cache.PhysicalRow(product.cache.Slot(position), bank) * reference.pitch_bytes;
        reference.height = reference.width;
    }
    const explore::ExploreRenderedCardProbe probe{
        .reference = reference,
        .content_x = card ? card->image_x + content_x : 0U,
        .content_y = card ? card->image_y + content_y : 0U,
        .content_width = content_width,
        .content_height = content_height,
    };
    if (sample_card && explore::sample_explore_rendered_card(checksum_target, count_target, reference, device_facts + kCardSamplesOffset, stream) !=
                           explore::kExploreStorageSuccess)
        throw std::runtime_error("Explore rendered card diagnostic sampling failed");
    auto rendered_probe = probe;
    if (card && probe_annotation) {
        rendered_probe.box_x = card->image_x + static_cast<std::uint32_t>(probe_annotation->box_xyxy[0] * static_cast<float>(card->image_width));
        rendered_probe.box_y = card->image_y + static_cast<std::uint32_t>(probe_annotation->box_xyxy[1] * static_cast<float>(card->image_height));
        const auto box_right = card->image_x + static_cast<std::uint32_t>(probe_annotation->box_xyxy[2] * static_cast<float>(card->image_width));
        const auto box_bottom = card->image_y + static_cast<std::uint32_t>(probe_annotation->box_xyxy[3] * static_cast<float>(card->image_height));
        rendered_probe.box_width = box_right > rendered_probe.box_x ? box_right - rendered_probe.box_x : 0U;
        rendered_probe.box_height = box_bottom > rendered_probe.box_y ? box_bottom - rendered_probe.box_y : 0U;
        const bool padded = static_cast<std::uint64_t>(rendered_probe.content_width) * rendered_probe.content_height <
                            static_cast<std::uint64_t>(checksum_target.width) * checksum_target.height;
        const bool full_card =
            card->image_x == 0U && card->image_y == 0U && card->image_width == checksum_target.width && card->image_height == checksum_target.height;
        const bool identity_preview = !product.plan.augmentation.enabled || !product.plan.augmentation_config.enabled;
        const bool probe_valid = rendered_probe.reference.valid() && identity_preview && padded && full_card && rendered_probe.box_width > 2U &&
                                 rendered_probe.box_height > 2U && rendered_probe.box_x < checksum_target.width &&
                                 rendered_probe.box_y < checksum_target.height && rendered_probe.box_width <= checksum_target.width - rendered_probe.box_x &&
                                 rendered_probe.box_height <= checksum_target.height - rendered_probe.box_y;
        if (probe_valid) {
            if (explore::probe_explore_rendered_card(checksum_target, count_target, rendered_probe, device_facts + 2U, stream) !=
                explore::kExploreStorageSuccess)
                throw std::runtime_error("Explore rendered card diagnostic probe failed");
        } else {
            probe_annotation.reset();
        }
    }
    probes_[probe_count_++] = {.generation = generation,
                               .slot = slot,
                               .compiled_index = compiled_index,
                               .count_target = count_target,
                               .checksum_target = checksum_target,
                               .probe = probe,
                               .seed = product.plan.augmentation.seed,
                               .augmented = product.plan.augmentation.enabled,
                               .card = card.has_value(),
                               .probe_annotation = probe_annotation.has_value(),
                               .dataset_identity = product.plan.dataset_identity,
                               .image_key = sample_card ? sample_card->erasure.key : 0U,
                               .source_extent = {product.store->header().image_width, product.store->header().image_height},
                               .sampled_card = sample_card != nullptr,
                               .augmentation_config_enabled = product.plan.augmentation_config.enabled};
} catch (...) {
    // Optional diagnostics may fail independently of the rendered product.
    // Partial launches retain their buffers until normal stream settlement.
    probes_disabled_ = true;
    probe_count_ = 0U;
    diagnostics_.Emit([&] {
        return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                    .operation = VisualDiagnosticOperation::ExploreProbeFailed,
                                    .device = device_,
                                    .generation = generation};
    });
}
bool GalleryStreamProbe::InitializeProbes() noexcept {
    if (diagnostic_stream_ != nullptr && probes_ready_ != nullptr) return true;
    if (diagnostic_stream_ == nullptr && cudaStreamCreateWithFlags(&diagnostic_stream_, cudaStreamNonBlocking) != cudaSuccess) {
        probes_disabled_ = true;
        return false;
    }
    if (probes_ready_ == nullptr && cudaEventCreateWithFlags(&probes_ready_, cudaEventDisableTiming) != cudaSuccess) {
        probes_disabled_ = true;
        return false;
    }
    return true;
}
void GalleryStreamProbe::FlushProbes(const std::uintptr_t stream) {
    if (probe_count_ == 0U) return;
    submitted_probes_ = std::exchange(probe_count_, 0U);
    auto* const cuda_stream = reinterpret_cast<cudaStream_t>(stream);
    probes_pending_.store(true, std::memory_order_release);
    if (cudaEventRecord(probes_ready_, cuda_stream) != cudaSuccess || cudaStreamWaitEvent(diagnostic_stream_, probes_ready_, 0U) != cudaSuccess ||
        cudaMemcpyAsync(storage_.buffers_.semantic_count_pinned_.data(), storage_.buffers_.semantic_count_device_.data(),
                        submitted_probes_ * kProbeFacts * sizeof(std::uint64_t), cudaMemcpyDeviceToHost, diagnostic_stream_) != cudaSuccess) {
        probes_disabled_ = true;
        return;
    }
    if (acceptance_) acceptance_->ObserveSubmission(reinterpret_cast<std::uintptr_t>(diagnostic_stream_), ExploreAcceptanceGate::SubmissionStage::Probe);
    if (cudaLaunchHostFunc(diagnostic_stream_, [](void* owner) { static_cast<GalleryStreamProbe*>(owner)->CollectProbes(); }, this) != cudaSuccess)
        probes_disabled_ = true;
    else {
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::ExploreProbeBatchSubmitted,
                                        .device = device_,
                                        .generation = probes_[0U].generation,
                                        .value = submitted_probes_,
                                        .context = {.staging_bytes = submitted_probes_ * kProbeFacts * sizeof(std::uint64_t)}};
        });
    }
}
void GalleryStreamProbe::CollectProbes() noexcept {
    const auto* facts = static_cast<const std::uint64_t*>(storage_.buffers_.semantic_count_pinned_.data());
    for (std::size_t index = 0U; index < submitted_probes_; ++index) EmitProbe(probes_[index], facts + index * kProbeFacts);
    probes_pending_.store(false, std::memory_order_release);
}
void GalleryStreamProbe::EmitProbe(const RenderedProbe& record, const std::uint64_t* facts) const noexcept {
    const auto& [generation, slot, compiled_index, count_target, checksum_target, probe, seed, augmented, card, probe_annotation, dataset_identity, image_key,
                 source_extent, sampled_card, augmentation_config_enabled] = record;
    diagnostics_.Emit([&] {
        return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                    .operation = VisualDiagnosticOperation::ExploreSemanticPixels,
                                    .device = device_,
                                    .generation = generation,
                                    .value = slot,
                                    .detail = facts[0],
                                    .context = {.capacity_width = count_target.width, .capacity_height = count_target.height}};
    });
    diagnostics_.Emit([&] {
        return VisualDiagnosticFact{
            .system = contracts::DiagnosticOwner::Explore,
            .operation = VisualDiagnosticOperation::ExploreImagePixels,
            .device = device_,
            .generation = generation,
            .value = compiled_index,
            .detail = facts[1],
            .context = {.capacity_width = static_cast<std::uint32_t>(seed),
                        .capacity_height = static_cast<std::uint32_t>(slot),
                        .staging_bytes = (checksum_target.width == checksum_target.height ? 1U : 0U) | (augmented ? 2U : 0U) | (card ? 4U : 0U)}};
    });
    if (sampled_card) EmitCardSamples(record, facts);
    if (!card) return;
    const auto packed_content = (static_cast<std::uint64_t>(probe.content_x & 0xffffU) << 48U) |
                                (static_cast<std::uint64_t>(probe.content_y & 0xffffU) << 32U) |
                                (static_cast<std::uint64_t>(probe.content_width & 0xffffU) << 16U) | (probe.content_height & 0xffffU);
    diagnostics_.Emit([&] {
        auto fact = Diagnostic(VisualDiagnosticOperation::ExploreCardGeometryProbe, generation);
        fact.value = slot;
        fact.detail = compiled_index;
        fact.context.capacity_width = checksum_target.width;
        fact.context.capacity_height = checksum_target.height;
        fact.context.staging_bytes = packed_content;
        return fact;
    });
    if (probe_annotation) {
        const auto flags =
            (facts[2] != 0U ? 1U : 0U) | (facts[3] != 0U ? 2U : 0U) | (facts[4] != 0U ? 4U : 0U) | (facts[5] != 0U ? 8U : 0U) | (facts[6] != 0U ? 16U : 0U);
        diagnostics_.Emit([&] {
            auto fact = Diagnostic(VisualDiagnosticOperation::ExploreRenderedCardProbe, generation);
            fact.value = slot;
            fact.detail = compiled_index;
            fact.context.capacity_width = static_cast<std::uint32_t>(flags);
            fact.context.capacity_height = static_cast<std::uint32_t>(std::min<std::uint64_t>(facts[2], std::numeric_limits<std::uint32_t>::max()));
            fact.context.staging_bytes = (std::min<std::uint64_t>(facts[3], std::numeric_limits<std::uint32_t>::max()) << 32U) |
                                         (std::min<std::uint64_t>(facts[4], 0xffffU) << 16U) | std::min<std::uint64_t>(facts[5], 0xffffU);
            fact.context.source = {.source_width = checksum_target.width,
                                   .source_height = checksum_target.height,
                                   .content_x = probe.content_x,
                                   .content_y = probe.content_y,
                                   .content_width = probe.content_width,
                                   .content_height = probe.content_height};
            return fact;
        });
        const auto transition_count =
            (probe.content_y != 0U ? probe.content_width : 0U) + (probe.content_y + probe.content_height < checksum_target.height ? probe.content_width : 0U) +
            (probe.content_x != 0U ? probe.content_height : 0U) + (probe.content_x + probe.content_width < checksum_target.width ? probe.content_height : 0U);
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{.system = contracts::DiagnosticOwner::Explore,
                                        .operation = VisualDiagnosticOperation::ExploreRenderedTransitionProbe,
                                        .device = device_,
                                        .generation = generation,
                                        .value = slot,
                                        .detail = compiled_index,
                                        .context = {.capacity_width = static_cast<std::uint32_t>(facts[3]),
                                                    .capacity_height = static_cast<std::uint32_t>(facts[6]),
                                                    .staging_bytes = transition_count}};
        });
    }
}
void GalleryStreamProbe::EmitCardSamples(const RenderedProbe& record, const std::uint64_t* facts) const noexcept try {
    if (!diagnostics_.pixel_probes_enabled()) return;
    constexpr explore::ExploreRenderedCardSampleGrid grid{};
    const auto& clean = record.checksum_target;
    const auto& reference = record.probe.reference;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << "{\"dataset_identity\":" << record.dataset_identity << ",\"augmentation_seed\":" << record.seed << ",\"image_key\":" << record.image_key
           << ",\"compiled_index\":" << record.compiled_index << ",\"slot\":" << record.slot
           << ",\"augmentation_enabled\":" << (record.augmented ? "true" : "false")
           << ",\"augmentation_config_enabled\":" << (record.augmentation_config_enabled ? "true" : "false") << ",\"source_width\":" << record.source_extent[0]
           << ",\"source_height\":" << record.source_extent[1] << ",\"width\":" << clean.width << ",\"height\":" << clean.height
           << ",\"reference_width\":" << reference.width << ",\"reference_height\":" << reference.height
           << ",\"reference_present\":" << (reference.data != nullptr ? "true" : "false") << ",\"checksum\":" << facts[1] << ",\"axis_percent\":[";
    for (std::size_t axis = 0U; axis != grid.kAxisCount; ++axis) {
        if (axis != 0U) output << ',';
        output << grid.percent[axis];
    }
    // Arrays share row-major grid order. Each RGBA integer has R in its low byte.
    // Dimensions and integer coordinates distinguish thumbnail and final-card
    // samples even when those physical targets have different extents.
    const auto coordinates = [&](const char* name, const std::uint32_t extent) {
        output << "],\"" << name << "\":[";
        for (std::size_t axis = 0U; axis != grid.kAxisCount; ++axis) {
            if (axis != 0U) output << ',';
            output << static_cast<std::uint64_t>(grid.percent[axis]) * extent / 100U;
        }
    };
    coordinates("x", clean.width);
    coordinates("y", clean.height);
    coordinates("reference_x", reference.width);
    coordinates("reference_y", reference.height);
    const auto pixels = [&](const char* name, const std::size_t word, const unsigned int shift) {
        output << "],\"" << name << "\":[";
        for (std::size_t sample = 0U; sample != grid.kSampleCount; ++sample) {
            if (sample != 0U) output << ',';
            output << static_cast<std::uint32_t>(facts[kCardSamplesOffset + sample * grid.kWordsPerSample + word] >> shift);
        }
    };
    pixels("clean_rgba", 0U, 0U);
    pixels("semantic_rgba", 0U, 32U);
    pixels("reference_rgba", 1U, 0U);
    output << "]}";
    const auto payload = std::move(output).str();
    auto fact = Diagnostic(VisualDiagnosticOperation::ExploreCardPixelSamples, record.generation);
    fact.value = record.slot;
    fact.detail = record.compiled_index;
    fact.context.capacity_width = clean.width;
    fact.context.capacity_height = clean.height;
    fact.failure_detail = payload;
    diagnostics_(fact);
} catch (...) {
    // Diagnostic serialization runs on the existing probe completion callback.
}
auto GalleryStreamProbe::ReleaseProbesChecked(decltype(&cudaStreamSynchronize) stream_wait_, cudaError_t& unsettled) noexcept
    -> mmltk::frameworks::gpu::SystemImageModel::Release {
    std::exception_ptr failure;
    const auto record = [&](const cudaError_t status, const char* message) {
        if (status == cudaSuccess) return;
        try {
            throw std::runtime_error(message);
        } catch (...) { failure = mmltk::frameworks::gpu::combine_image_failures(failure, std::current_exception()); }
    };
    if (diagnostic_stream_) {
        const auto settled = stream_wait_(diagnostic_stream_);
        record(settled, "Explore diagnostic retirement settlement failed");
        if (settled != cudaSuccess) {
            unsettled = settled;
            return {.all_released = false, .failure = failure};
        }
    }
    const bool owned = probes_ready_ || diagnostic_stream_;
    if (probes_ready_) {
        const auto released = cudaEventDestroy(probes_ready_);
        record(released, "Explore diagnostic event release failed");
        if (released == cudaSuccess) probes_ready_ = nullptr;
    }
    if (diagnostic_stream_) {
        const auto released = cudaStreamDestroy(diagnostic_stream_);
        record(released, "Explore diagnostic stream release failed");
        if (released == cudaSuccess) diagnostic_stream_ = nullptr;
    }
    const bool all_released = !probes_ready_ && !diagnostic_stream_;
    if (owned && all_released)
        diagnostics_.Emit([&] {
            return VisualDiagnosticFact{
                .system = contracts::DiagnosticOwner::Explore, .operation = VisualDiagnosticOperation::ExploreProbeResourcesReleased, .device = device_};
        });
    return {.all_released = all_released, .failure = failure};
}
namespace {
[[nodiscard]] std::uint32_t diagnostic_coordinate(const float value) noexcept { return static_cast<std::uint32_t>(std::clamp(value, 0.0F, 1.0F) * 65535.0F); }
}  // namespace
void GalleryStreamProbe::DiagnoseDescriptors(const GalleryDescriptorStorage& descriptors_, VisualDiagnosticSink diagnostics_, int device_,
                                             const std::uint64_t generation, const std::uint64_t slot, const std::size_t first, const std::size_t count,
                                             const std::size_t rle_count) {
    if (!diagnostics_.valid()) return;
    float minimum_x = 1.0F;
    float minimum_y = 1.0F;
    float maximum_x = 0.0F;
    float maximum_y = 0.0F;
    for (std::size_t local = 0U; local != count; ++local) {
        const auto annotation = load_payload<explore::ExploreRenderAnnotationDescriptor>(
            descriptors_.storage_.buffers_.descriptors_.data(),
            descriptors_.descriptor_layout_.annotations.offset + (first + local) * sizeof(explore::ExploreRenderAnnotationDescriptor));
        minimum_x = std::min(minimum_x, annotation.box_xyxy[0]);
        minimum_y = std::min(minimum_y, annotation.box_xyxy[1]);
        maximum_x = std::max(maximum_x, annotation.box_xyxy[2]);
        maximum_y = std::max(maximum_y, annotation.box_xyxy[3]);
    }
    diagnostics_.Emit([&] {
        return VisualDiagnosticFact{
            .system = contracts::DiagnosticOwner::Explore,
            .operation = VisualDiagnosticOperation::ExploreTransformedBounds,
            .device = device_,
            .generation = generation,
            .value = slot,
            .detail = (static_cast<std::uint64_t>(count) << 32U) | rle_count,
            .context = {
                .capacity_width = count == 0U ? 0U : diagnostic_coordinate(minimum_x),
                .capacity_height = count == 0U ? 0U : diagnostic_coordinate(minimum_y),
                .staging_bytes = count == 0U ? 0U : (static_cast<std::size_t>(diagnostic_coordinate(maximum_x)) << 32U) | diagnostic_coordinate(maximum_y)}};
    });
}
void GalleryStreamProbe::DiagnosePreparedImage(const ExploreRenderPlan& plan, const GalleryReadScheduler& scheduler_,
                                               const mmltk::backend::models::rfdetr::GpuAugmentationExecutor& augmenter, VisualDiagnosticSink diagnostics_,
                                               const GalleryReadScheduler::Lane& lane, const std::size_t image, const std::size_t staging_slot) noexcept try {
    if (!diagnostics_.valid()) return;
    const auto payload = "{\"dataset_identity\":" + std::to_string(plan.dataset_identity) + ",\"augmentation_seed\":" + std::to_string(plan.augmentation.seed) +
                         ",\"compiled_index\":" + std::to_string(lane.compiled_index) + ",\"slot\":" + std::to_string(lane.destination_slot) +
                         ",\"lane\":" + std::to_string(lane.index) +
                         ",\"output_domain\":\"UnitRgb\",\"prepared\":" + augmenter.prepared_image_diagnostic(image, staging_slot) + "}";
    auto fact = scheduler_.LaneDiagnostic(VisualDiagnosticOperation::ExploreAugmentationImagePrepared, lane);
    fact.failure_detail = payload;
    diagnostics_(fact);
} catch (...) {
    // Collecting optional plan details cannot change augmentation submission.
}
}  // namespace mmltk::controller::explore_detail
