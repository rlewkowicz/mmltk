#pragma once  // backend.data private implementation boundary
#include "src/common/io/scoped_fd.h"
#include <atomic>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <nlohmann/json.hpp>
#include "src/common/concurrency/cancellation_observation.h"
namespace mmltk::backend::data::benchmark_internal {
inline constexpr std::uint32_t kBenchmarkCacheSchemaVersion = 2U;
using BenchmarkTraceSink = std::function<void(std::string_view, const nlohmann::json&)>;
struct BenchmarkCacheLayout {
    std::filesystem::path root;
    std::filesystem::path downloads;
    std::filesystem::path images;
    std::filesystem::path indexes;
    std::filesystem::path locks;
    [[nodiscard]] static BenchmarkCacheLayout create(const std::filesystem::path& root);
    [[nodiscard]] std::filesystem::path source_downloads(std::string_view source) const;
    [[nodiscard]] std::filesystem::path source_images(std::string_view source) const;
    [[nodiscard]] std::filesystem::path source_indexes(std::string_view source) const;
};
class ArtifactLease {
   public:
    ArtifactLease() = default;
    ArtifactLease(const ArtifactLease&) = delete;
    ArtifactLease& operator=(const ArtifactLease&) = delete;
    ArtifactLease(ArtifactLease&& other) noexcept;
    ArtifactLease& operator=(ArtifactLease&& other) noexcept;
    ~ArtifactLease();
    [[nodiscard]] static ArtifactLease acquire(const std::filesystem::path& lock_path, mmltk::common::concurrency::CancellationObservation cancellation);

   private:
    explicit ArtifactLease(int descriptor) noexcept;
    void release() noexcept;
    mmltk::common::io::ScopedFd descriptor_;
};
void throw_if_benchmark_cancelled(mmltk::common::concurrency::CancellationObservation cancellation);
void write_json_atomically(const std::filesystem::path& path, const nlohmann::json& value, mmltk::common::concurrency::CancellationObservation cancellation);
[[nodiscard]] nlohmann::json read_json_file(const std::filesystem::path& path);
[[nodiscard]] bool is_safe_cache_component(std::string_view value) noexcept;
template <class Builder>
    requires std::invocable<Builder> && std::convertible_to<std::invoke_result_t<Builder>, nlohmann::json>
inline void trace_benchmark_event(const BenchmarkTraceSink& sink, const std::string_view event, Builder&& fields) {
    if (!sink) { return; }
    sink(event, std::invoke(std::forward<Builder>(fields)));
}
void remove_cache_path(const std::filesystem::path& path);
}  // namespace mmltk::backend::data::benchmark_internal
