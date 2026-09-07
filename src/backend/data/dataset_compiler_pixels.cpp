#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/image_resize.h"

// CLEANUP-IGNORE: This global module fragment declares the direct vendor and standard headers required by this
// implementation.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "detail/writable_pixel_range.h"
#include "src/common/concurrency/event_cancellation.h"
#include "src/common/concurrency/parallel_range.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/io/file_memory.h"
#include "src/common/math/checked_arithmetic.h"
#include "stb_image.h"

import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;

#include "detail/dataset_compiler_internal.h"

namespace mmltk::backend::data::compiler_internal {

using detail::WritablePixelRange;
using mmltk::common::concurrency::parallel_for_range_indexed;
using mmltk::common::io::FileHandle;
using mmltk::common::math::checked_cast;

namespace {

void hwc_uint8_to_nchw_float(const uint8_t* src, float* dst, int height, int width) {
    mmltk::common::logging::ScopedProfile profile{"compiler.pixels.convert.avx2"};
    rgb_hwc_u8_to_nchw_f32(src, dst, checked_cast<uint32_t>(width, "image width too large"),
                           checked_cast<uint32_t>(height, "image height too large"));
}

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void decode_pixel_image(const std::filesystem::path& split_dir, const WritablePixelRange& pixel_blob, uint32_t image_index,
                        uint32_t target_width, uint32_t target_height, RgbImageResizer& image_resizer, size_t image_stride,
                        std::vector<uint8_t>& resize_scratch) {
    const int width = checked_cast<int>(target_width, "image width too large");
    const int height = checked_cast<int>(target_height, "image height too large");

    const std::filesystem::path img_path = image_path(split_dir, image_index);
    float* dst = pixel_blob.image(image_index, image_stride);

    int raw_width = 0;
    int raw_height = 0;
    int raw_channels = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> raw_pixels(nullptr, &stbi_image_free);
    {
        mmltk::common::logging::ScopedProfile profile{"compiler.pixels.load_png"};
        raw_pixels.reset(stbi_load(img_path.c_str(), &raw_width, &raw_height, &raw_channels, 3));
    }
    if (!raw_pixels) { throw std::runtime_error("failed to load image file: " + img_path.string()); }

    const uint32_t source_width = checked_cast<uint32_t>(raw_width, "image width too large");
    const uint32_t source_height = checked_cast<uint32_t>(raw_height, "image height too large");
    if (source_width == target_width && source_height == target_height) {
        mmltk::common::logging::profile_add_value("compiler.pixels.convert_bytes", static_cast<size_t>(width) * height * 3U);
        hwc_uint8_to_nchw_float(raw_pixels.get(), dst, height, width);
        return;
    }
    const RgbLetterbox letterbox = compute_rgb_letterbox(source_width, source_height, target_width, target_height);
    const uint8_t* src_pixels = raw_pixels.get();
    if (static_cast<uint32_t>(raw_width) != letterbox.resized_width || static_cast<uint32_t>(raw_height) != letterbox.resized_height) {
        const size_t resized_bytes = static_cast<size_t>(letterbox.resized_width) * letterbox.resized_height * 3U;
        if (resize_scratch.capacity() < resized_bytes) {
            mmltk::common::logging::profile_add_value("compiler.pixels.resize_scratch_grows", 1);
            mmltk::common::logging::profile_add_value("compiler.pixels.resize_scratch_bytes", resized_bytes);
        }
        {
            mmltk::common::logging::ScopedProfile profile{"compiler.pixels.resize"};
            resize_scratch.resize(resized_bytes);
            image_resizer.resize(raw_pixels.get(), raw_width, raw_height, resize_scratch.data(),
                                 checked_cast<int>(letterbox.resized_width, "letterbox width too large"),
                                 checked_cast<int>(letterbox.resized_height, "letterbox height too large"));
        }
        mmltk::common::logging::profile_add_value("compiler.pixels.resize_count", 1);
        src_pixels = resize_scratch.data();
    }

    const bool has_padding = letterbox.resized_width != target_width || letterbox.resized_height != target_height ||
                             letterbox.offset_x != 0U || letterbox.offset_y != 0U;
    if (has_padding) {
        mmltk::common::logging::profile_add_value("compiler.pixels.letterbox_count", 1);
        {
            mmltk::common::logging::ScopedProfile profile{"compiler.pixels.convert.letterbox_avx2"};
            letterboxed_rgb_hwc_u8_to_nchw_f32(src_pixels, dst, letterbox.resized_width, letterbox.resized_height, target_width,
                                               target_height, letterbox.offset_x, letterbox.offset_y);
        }
    } else {
        hwc_uint8_to_nchw_float(src_pixels, dst, height, width);
    }

    mmltk::common::logging::profile_add_value("compiler.pixels.convert_bytes", static_cast<size_t>(width) * height * 3);
}
// NOLINTEND(bugprone-easily-swappable-parameters)

// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void decode_pixel_worker(const std::filesystem::path& split_dir, const WritablePixelRange& pixel_blob, std::atomic<uint32_t>& next_image,
                         uint32_t num_images, uint32_t target_width, uint32_t target_height, size_t image_stride,
                         int resize_threads_per_image, ProgressCounter* completed_images,
                         mmltk::common::concurrency::CancellationObservation cancel_requested, std::atomic<bool>* failure_requested) {
    mmltk::common::logging::ScopedProfile profile{"compiler.pixels.decode_worker"};
    RgbImageResizer image_resizer(resize_threads_per_image);
    std::vector<uint8_t> resize_scratch;
    ProgressBatch progress(completed_images);
    while (true) {
        if ((cancel_requested.requested()) || (failure_requested != nullptr && failure_requested->load(std::memory_order_relaxed))) {
            break;
        }
        const uint32_t image_index = next_image.fetch_add(1, std::memory_order_relaxed);
        if (image_index >= num_images) { break; }
        try {
            decode_pixel_image(split_dir, pixel_blob, image_index, target_width, target_height, image_resizer, image_stride,
                               resize_scratch);
            progress.increment();
        } catch (...) {
            if (failure_requested != nullptr) { failure_requested->store(true, std::memory_order_relaxed); }
            throw;
        }
    }
}
// NOLINTEND(bugprone-easily-swappable-parameters)

}  // namespace

void write_pixel_blob(const FileHandle& fd, const PixelBlobWriteRequest& request) {
    mmltk::common::logging::ScopedProfile profile{"compiler.pixels.write_blob"};
    const size_t pixel_bytes = static_cast<size_t>(request.num_images) * request.image_stride;
    mmltk::common::logging::profile_set_value("compiler.pixels.total_bytes", pixel_bytes);
    WritablePixelRange pixel_blob(fd.get(), request.pixel_offset, pixel_bytes);

    if (request.num_images == 0) { return; }

    const ResizeWorkerPlan resize_plan = plan_rgb_resize_workers(request.num_workers, request.any_resize, request.any_downscale);
    mmltk::common::logging::profile_set_value("compiler.pixels.worker.count", static_cast<size_t>(resize_plan.image_workers));
    mmltk::common::logging::profile_set_value("compiler.pixels.image_workers", static_cast<size_t>(resize_plan.image_workers));
    mmltk::common::logging::profile_set_value("compiler.pixels.resize_threads_per_image",
                                              static_cast<size_t>(resize_plan.resize_threads_per_image));
    std::atomic<uint32_t> next_image{0};

    parallel_for_range_indexed<int>(0, resize_plan.image_workers, resize_plan.image_workers, request.worker_cpus,
                                    [&](const int worker, int, int) {
                                        if (worker >= request.initial_active_workers && request.release_all_workers != nullptr) {
                                            while (!request.release_all_workers->load(std::memory_order_acquire)) {
                                                request.release_all_workers->wait(false, std::memory_order_acquire);
                                            }
                                        }
                                        decode_pixel_worker(request.split_dir, pixel_blob, next_image, request.num_images, request.width,
                                                            request.height, request.image_stride, resize_plan.resize_threads_per_image,
                                                            request.completed_images, request.cancel_requested, request.failure_requested);
                                    });
}

}  // namespace mmltk::backend::data::compiler_internal
