#pragma once
#include "src/backend/data/benchmark/detail/benchmark_image_decoder.h"
#include "src/backend/data/benchmark/detail/benchmark_resources.h"
#include <span>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
namespace mmltk::backend::data::benchmark_internal {
class ArtifactLease;
// Ordinary source generation: copied by readers, invalidated before replacement.
// This identity is product lifetime state, independent of diagnostic identities.
using BenchmarkSourceGeneration = std::uint64_t;
struct BenchmarkImageGeometry {
 std::filesystem::path root;
 std::uint64_t image_id = 0;
 BenchmarkSourceGeneration generation = 0;
 std::uint32_t width = 0, height = 0;
};
// The backing and its capacity grant follow the last reader. Producers may
// supply a pooled vector or an opened mapping without copying encoded bytes.
struct BenchmarkEncodedImage {
 BenchmarkAllowance allowance;
 std::shared_ptr<const void> backing;
 std::span<const std::uint8_t> encoded;
 BenchmarkImageHeader header;
 // Same published inode, mapped before its staging descriptor closes. Used
 // under pressure when retaining the pooled input would block its consumer.
 std::shared_ptr<const BenchmarkEncodedImage> file_backing;
};
// Emitted only after admitted cache reuse or atomic image publication.
// Physical custody and generation come from the captured publication ticket.
struct CachedImageReady {
 std::uint64_t image_id = 0;
 std::optional<std::pair<std::uint32_t, std::uint32_t>> dimensions{};
 bool defer_pixels = false;
 std::shared_ptr<const BenchmarkEncodedImage> payload;
};
// Captures physical source, generation, attempt and lease once. Copies retain
// custody, never a writer or a runnable execution owner. Delivery after source
// replacement or attempt retirement cannot attach to new registrations.
class BenchmarkSourcePublication final {
public:
 BenchmarkSourcePublication() = default;
 void operator()(const CachedImageReady&) const;
 // A repair with a canonical destination uses the actual pixel consumption as
 // its acceptance. Returns false when this ticket has no registered consumer.
 [[nodiscard]] bool consume(const CachedImageReady&) const;
 void geometry_ready(std::uint64_t, std::pair<std::uint32_t, std::uint32_t>) const;
 [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(state_); }
private:
 friend class BenchmarkCompilePipeline;
 struct State;
 explicit BenchmarkSourcePublication(std::shared_ptr<const State> state) : state_(std::move(state)) {}
 std::shared_ptr<const State> state_;
};
using CachedImageReadySink = std::function<void(const CachedImageReady&)>;
}  // namespace mmltk::backend::data::benchmark_internal
