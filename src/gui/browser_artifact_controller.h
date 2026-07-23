#pragma once

#include "browser/host_api_protocol.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mmltk::runtime {
class BackgroundExecutor;
class UiCallbackQueue;
}  

namespace mmltk::gui {

[[nodiscard]] std::string canonical_weight_cache_path(std::string_view preset_name);

class BrowserArtifactController {
   public:
    using Completion = std::function<void(const std::string&)>;
    using TraceSink = std::function<void(std::string_view, const nlohmann::json&)>;

    BrowserArtifactController(mmltk::runtime::BackgroundExecutor& executor,
                              mmltk::runtime::UiCallbackQueue& ui_callbacks);
    ~BrowserArtifactController();

    BrowserArtifactController(const BrowserArtifactController&) = delete;
    BrowserArtifactController& operator=(const BrowserArtifactController&) = delete;

    void inspect_dataset(std::string train_path, std::string val_path, std::string test_path, std::string preset_name,
                         std::uint32_t resolution);
    void compile_dataset(std::string source_dir, std::string output_dir, std::string preset_name,
                         std::uint32_t resolution, bool overwrite, Completion on_complete);
    void cancel_dataset_compile();
    void acquire_weight(std::string preset_name, Completion on_complete);
    void cancel_weight_acquisition();
    void shutdown();
    void set_trace_sink(TraceSink sink);

    [[nodiscard]] mmltk::browser::ArtifactState snapshot() const;

   private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  
