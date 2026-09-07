#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>

#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/model.h"
#include "src/controller/services/artifact_store.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/local_run.h"
#include "src/controller/subsystems/system/system_events.h"

// CLEANUP-IGNORE: Model events form a canonical reflected vocabulary distinct from Dataset artifact progress and
// terminal facts.
namespace mmltk::controller {

struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] ModelProgressChanged final {
    std::uint64_t generation = 0U;
    bool active = false;
    contracts::ModelSelectionResult terminal{};
    contracts::ModelProgress progress{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] ModelChanged final {
    contracts::ModelUiState snapshot{};
};

class ModelRuntime {
   public:
    virtual ~ModelRuntime() = default;
    [[nodiscard]] virtual std::string Acquire(const contracts::ModelSelectionKey&, const std::filesystem::path&, std::stop_token,
                                              const std::function<void(const contracts::ModelProgress&)>&) = 0;
};

class ArtifactModelRuntime final : public ModelRuntime {
   public:
    ArtifactModelRuntime() = default;
    explicit ArtifactModelRuntime(services::ArtifactStore store) : store_(std::move(store)) {}
    [[nodiscard]] std::string Acquire(const contracts::ModelSelectionKey&, const std::filesystem::path&, std::stop_token,
                                      const std::function<void(const contracts::ModelProgress&)>&) override;

   private:
    // CLEANUP-IGNORE: The model runtime owns an artifact store, while the dataset runtime additionally owns typed
    // compiler diagnostics; merging the sealed runtimes would erase their domain boundaries.
    services::ArtifactStore store_;
};

class ModelSystem final {
   public:
    using event_type = std::variant<ModelProgressChanged, ModelChanged>;
    using RuntimeFactory = std::function<std::unique_ptr<ModelRuntime>()>;
    // CLEANUP-IGNORE: ModelSystem is a sealed typed selection system; Dataset and compute systems retain their own
    // direct intent and snapshot APIs.
    ModelSystem(SettingsSystem&, RuntimeFactory, SystemEventSink<event_type> = {});
    ~ModelSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ModelUiState Select(contracts::ModelSelectionRequest);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] contracts::ModelUiState Stop() noexcept;
    void Shutdown() noexcept;
    [[= contracts::reflection::Snapshot{contracts::kModelUiStateByteBudget}]] [[nodiscard]] contracts::ModelUiState snapshot() const;
    [[nodiscard]] contracts::ModelSelection selection() const;

   private:
    void progress(const contracts::ModelProgress&) noexcept;
    [[nodiscard]] direct::LocalRun::Notification changed(contracts::ModelUiState);

    SettingsSystem& settings_;
    RuntimeFactory factory_;
    SystemEventSink<event_type> events_;
    mutable std::mutex mutex_;
    contracts::ModelUiState state_{};
    std::unique_ptr<ModelRuntime> runtime_;
    direct::LocalRun run_;
};

MMLTK_REFLECT_FIELDS(ModelProgressChanged)
MMLTK_REFLECT_FIELDS(ModelChanged)

}  // namespace mmltk::controller
