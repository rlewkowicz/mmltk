#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::backend::ml::runtime {

enum class TensorRtProfilingVerbosity : std::uint8_t {
    LayerNames,
    Detailed,
    Disabled,
};

struct TensorRtOptimizationProfile final {
    std::string input_name;
    std::vector<std::int64_t> minimum;
    std::vector<std::int64_t> optimum;
    std::vector<std::int64_t> maximum;
};

struct TensorRtEngineOptions final {
    std::int32_t device = 0;
    // Permission for automatic FP16 lowering; TensorRT 11 preserves model-defined types.
    bool allow_fp16 = true;
    bool allow_tf32 = true;
    std::uint64_t workspace_bytes = 1ULL << 30U;
    TensorRtProfilingVerbosity profiling_verbosity = TensorRtProfilingVerbosity::LayerNames;
    std::filesystem::path save_engine_path;
    std::vector<TensorRtOptimizationProfile> optimization_profiles;
    std::string context = "TensorRT";
    std::function<void(std::string_view)> log;
    std::function<bool()> continue_build;
};

namespace detail {
class TensorRtEngineAccess;
}

class TensorRtEngine final {
   public:
    TensorRtEngine(const std::filesystem::path& model_path, TensorRtEngineOptions options);
    ~TensorRtEngine();

    TensorRtEngine(const TensorRtEngine&) = delete;
    TensorRtEngine& operator=(const TensorRtEngine&) = delete;
    TensorRtEngine(TensorRtEngine&&) noexcept;
    TensorRtEngine& operator=(TensorRtEngine&&) noexcept;

    [[nodiscard]] std::int32_t device() const noexcept;
    [[nodiscard]] const std::filesystem::path& model_path() const noexcept;
    [[nodiscard]] bool built_from_onnx() const noexcept;
    [[nodiscard]] std::uintptr_t native_engine_handle() const noexcept;
    void Save(const std::filesystem::path& path) const;

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend class detail::TensorRtEngineAccess;
};

}  // namespace mmltk::backend::ml::runtime
