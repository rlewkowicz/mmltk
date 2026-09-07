#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>

#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/artifact.h"
#include "src/controller/services/artifact_store.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/local_run.h"
#include "src/controller/subsystems/system/system_events.h"

namespace mmltk::controller {

struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] DatasetProgress final {
    std::uint64_t generation = 0U;
    bool active = false;
    contracts::ArtifactTerminal terminal{};
    contracts::ArtifactProgress progress{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] DatasetChanged final {
    contracts::ArtifactUiState snapshot{};
};

class DatasetRuntime {
   public:
    virtual ~DatasetRuntime() = default;
    [[nodiscard]] virtual services::ArtifactCompileResult Compile(const services::ArtifactCompileRequest&, std::stop_token,
                                                                  const std::function<void(const contracts::ArtifactProgress&)>&) = 0;
    [[nodiscard]] virtual contracts::ArtifactInspection Inspect(const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>&,
                                                                std::string_view, std::uint32_t) = 0;
};
class ArtifactDatasetRuntime final : public DatasetRuntime {
   public:
    ArtifactDatasetRuntime();
    explicit ArtifactDatasetRuntime(services::ArtifactStore, services::ArtifactDiagnosticObserver = {});
    [[nodiscard]] services::ArtifactCompileResult Compile(const services::ArtifactCompileRequest&, std::stop_token,
                                                          const std::function<void(const contracts::ArtifactProgress&)>&) override;
    [[nodiscard]] contracts::ArtifactInspection Inspect(const std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>&,
                                                        std::string_view, std::uint32_t) override;

   private:
    services::ArtifactStore store_;
    services::ArtifactDiagnosticObserver diagnostics_{};
};

class DatasetSystem final {
   public:
    using event_type = std::variant<DatasetProgress, DatasetChanged>;
    using RuntimeFactory = std::function<std::unique_ptr<DatasetRuntime>()>;
    DatasetSystem(SettingsSystem&, RuntimeFactory, SystemEventSink<event_type> = {});
    ~DatasetSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ArtifactUiState Compile(
        contracts::WorkflowIntent<contracts::FeatureId::Train>);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ArtifactUiState Stop() noexcept;
    void Shutdown() noexcept;
    [[nodiscard]] contracts::ArtifactInspection Inspect(std::array<std::filesystem::path, contracts::kArtifactSplitCapacity>, std::string,
                                                        std::uint32_t);
    [[= contracts::reflection::Snapshot{contracts::kArtifactUiStateByteBudget}]] [[nodiscard]] contracts::ArtifactUiState snapshot() const;

   private:
    enum class DatasetActivity : std::uint8_t {
        None,
        Inspect,
        Compile,
    };
    void progress(const contracts::ArtifactProgress&) noexcept;
    [[nodiscard]] direct::LocalRun::Notification changed(std::optional<contracts::ArtifactUiState>);
    SettingsSystem& settings_;
    RuntimeFactory factory_;
    SystemEventSink<event_type> events_;
    mutable std::mutex mutex_;
    DatasetActivity activity_ = DatasetActivity::None;
    contracts::ArtifactUiState state_{};
    std::unique_ptr<DatasetRuntime> runtime_;
    direct::LocalRun run_;
};

MMLTK_REFLECT_FIELDS(DatasetProgress)
MMLTK_REFLECT_FIELDS(DatasetChanged)

}  // namespace mmltk::controller
