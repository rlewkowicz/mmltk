#pragma once
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
// Emitted only after admitted cache reuse or durable atomic image publication.
// The acquiring source lease remains held until its consumer drains readers.
struct CachedImageReady {
 std::filesystem::path root;
 std::uint64_t image_id = 0;
 std::shared_ptr<const ArtifactLease> custody{};
 // Source owners stamp their captured generation before execution admission.
 // Zero is an unstamped acquisition event and is never accepted by the executor.
 BenchmarkSourceGeneration generation = 0;
 std::optional<std::pair<std::uint32_t, std::uint32_t>> dimensions{};
 bool defer_pixels = false;
};
using CachedImageReadySink = std::function<void(const CachedImageReady&)>;
}  // namespace mmltk::backend::data::benchmark_internal
