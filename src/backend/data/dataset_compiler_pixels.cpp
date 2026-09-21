#include "src/backend/data/dataset_compiler.h"
#include "src/backend/imaging/resample/image_resize.h"
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
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void decode_pixel_image(const std::filesystem::path& split_dir, const WritablePixelRange& pixel_blob, uint32_t image_index, uint32_t target_width,
                        uint32_t target_height, mmltk::backend::imaging::resample::RgbImageResizer& image_resizer, size_t image_stride,
                        mmltk::backend::imaging::resample::ImageResizeMode resize_mode) {
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
 mmltk::backend::imaging::resample::ImageResizeGeometry geometry;
 {
  mmltk::common::logging::ScopedProfile profile{"compiler.pixels.resize"};
  geometry = image_resizer.resize_to_planar(
   {raw_pixels.get(),
    {source_width, source_height, static_cast<size_t>(source_width) * 3U, 0U, static_cast<size_t>(source_width) * source_height * 3U,
     mmltk::backend::imaging::resample::RgbPixelFormat::RGB8}},
   {dst,
    {target_width, target_height, static_cast<size_t>(target_width) * sizeof(float), static_cast<size_t>(target_width) * target_height * sizeof(float),
     image_stride, mmltk::backend::imaging::resample::RgbPixelFormat::PlanarUnitSrgbF32}},
   resize_mode);
 }
 if (source_width != geometry.resized_width || source_height != geometry.resized_height)
  mmltk::common::logging::profile_add_value("compiler.pixels.resize_count", 1);
 if (geometry.resized_width != target_width || geometry.resized_height != target_height)
  mmltk::common::logging::profile_add_value("compiler.pixels.letterbox_count", 1);
 mmltk::common::logging::profile_add_value("compiler.pixels.convert_bytes", static_cast<size_t>(width) * height * 3U);
}
// NOLINTEND(bugprone-easily-swappable-parameters)
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void decode_pixel_worker(const std::filesystem::path& split_dir, const WritablePixelRange& pixel_blob, std::atomic<uint32_t>& next_image, uint32_t num_images,
                         uint32_t target_width, uint32_t target_height, size_t image_stride, int resize_threads_per_image, bool perceptual_downscale,
                         ProgressCounter* completed_images, mmltk::common::concurrency::CancellationObservation cancel_requested,
                         std::atomic<bool>* failure_requested, mmltk::backend::imaging::resample::ImageResizeMode resize_mode) {
 mmltk::common::logging::ScopedProfile profile{"compiler.pixels.decode_worker"};
 mmltk::backend::imaging::resample::RgbImageResizer image_resizer(resize_threads_per_image, perceptual_downscale);
 ProgressBatch progress(completed_images);
 while (true) {
  if ((cancel_requested.requested()) || (failure_requested != nullptr && failure_requested->load(std::memory_order_relaxed))) { break; }
  const uint32_t image_index = next_image.fetch_add(1, std::memory_order_relaxed);
  if (image_index >= num_images) { break; }
  try {
   decode_pixel_image(split_dir, pixel_blob, image_index, target_width, target_height, image_resizer, image_stride, resize_mode);
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
 const mmltk::backend::imaging::resample::ResizeWorkerPlan resize_plan =
  mmltk::backend::imaging::resample::plan_rgb_resize_workers(request.num_workers, request.any_resize, request.any_downscale);
 mmltk::common::logging::profile_set_value("compiler.pixels.worker.count", static_cast<size_t>(resize_plan.image_workers));
 mmltk::common::logging::profile_set_value("compiler.pixels.image_workers", static_cast<size_t>(resize_plan.image_workers));
 mmltk::common::logging::profile_set_value("compiler.pixels.resize_threads_per_image", static_cast<size_t>(resize_plan.resize_threads_per_image));
 std::atomic<uint32_t> next_image{0};
 parallel_for_range_indexed<int>(0, resize_plan.image_workers, resize_plan.image_workers, request.worker_cpus, [&](const int worker, int, int) {
  if (worker >= request.initial_active_workers && request.release_all_workers != nullptr) {
   while (!request.release_all_workers->load(std::memory_order_acquire)) { request.release_all_workers->wait(false, std::memory_order_acquire); }
  }
  decode_pixel_worker(request.split_dir, pixel_blob, next_image, request.num_images, request.width, request.height, request.image_stride,
                      resize_plan.resize_threads_per_image, request.perceptual_downscale, request.completed_images, request.cancel_requested,
                      request.failure_requested, request.resize_mode);
 });
}
}  // namespace mmltk::backend::data::compiler_internal
