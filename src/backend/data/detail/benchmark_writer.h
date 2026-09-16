#pragma once  // backend.data private implementation boundary
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/image_resize.h"
#include "src/common/concurrency/cancellation_observation.h"
namespace mmltk::backend::data::benchmark_internal {
struct CachedImageSource {
    std::filesystem::path root;
};
struct EncodedImageRecord {
    std::uint64_t source_image_id = 0;
    std::uint32_t source_width = 0;
    std::uint32_t source_height = 0;
    std::uint32_t first_label = 0;
    std::uint16_t label_count = 0;
    std::uint16_t source_index = 0;
};
struct PreparedBenchmarkSplit {
    std::string name;
    std::vector<std::string> class_names;
    std::vector<CachedImageSource> sources;
    std::vector<EncodedImageRecord> images;
    std::vector<PackedInstance> labels;
    std::vector<RLEPair> rle_pairs;
};
class BenchmarkImageReadError final : public std::runtime_error {
   public:
    BenchmarkImageReadError(std::uint16_t source_index, std::uint64_t source_image_id, std::string detail);
    [[nodiscard]] std::uint16_t source_index() const noexcept;
    [[nodiscard]] std::uint64_t source_image_id() const noexcept;

   private:
    std::uint16_t source_index_ = 0U;
    std::uint64_t source_image_id_ = 0U;
};
[[nodiscard]] PackedInstance benchmark_letterbox_box(std::uint8_t class_id, float x1, float y1, float x2, float y2, const RgbLetterbox& letterbox);
struct BenchmarkWriteProgressEvent final {
    void* context = nullptr;
    void (*image_completed)(void*) = nullptr;
    void operator()() const {
        if (image_completed != nullptr) { image_completed(context); }
    }
    [[nodiscard]] explicit operator bool() const noexcept { return image_completed != nullptr; }
};
struct BenchmarkWriteRequest {
    const PreparedBenchmarkSplit& split;
    std::filesystem::path output_path;
    std::uint32_t resolution = 0;
    int num_workers = 0;
    std::span<const int> worker_cpus;
    bool overwrite = false;
    mmltk::common::concurrency::CancellationObservation cancel_requested = {};
    BenchmarkWriteProgressEvent progress;
    bool perceptual_downscale = false;
};
void write_benchmark_split(const BenchmarkWriteRequest& request);
}  // namespace mmltk::backend::data::benchmark_internal
