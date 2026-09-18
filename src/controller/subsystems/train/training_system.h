#pragma once
#include <filesystem>
#include "src/backend/models/rfdetr/training/checkpoint.h"
#include "src/common/system/execution_policy.h"
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/compute.h"
#include "src/controller/contracts/provider.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/services/vast_client.h"
#include "src/controller/services/train_process_client.h"
#include "src/backend/models/rfdetr/contract/training_metrics.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/runtime/local_run.h"
#include "src/controller/subsystems/system/model_system.h"
namespace mmltk::controller {
class TrainingRuntime {
   public:
    virtual ~TrainingRuntime() = default;
    [[nodiscard]] virtual mmltk::backend::models::rfdetr::TrainingCheckpointAdmission InspectCheckpoint(const std::filesystem::path&, std::stop_token);
    [[nodiscard]] virtual contracts::ComputeTerminal Train(mmltk::backend::models::rfdetr::TrainRequest, std::stop_token,
                                                           const std::function<void(const services::TrainProcessProgress&)>&) = 0;
    [[nodiscard]] virtual contracts::ProviderQueryResult Query(const contracts::ProviderPreferences&, std::stop_token) = 0;
    [[nodiscard]] virtual contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&,
                                                                 contracts::ProviderOfferIdentity, int instance_id, std::string_view launch_token,
                                                                 std::stop_token) = 0;
    [[nodiscard]] virtual contracts::ProviderEffectResult Reconcile(const services::VastReconciliationRequest&, std::stop_token) = 0;
};
struct NativeTrainingConfiguration final {
    services::VastProviderClient provider{};
    std::filesystem::path training_executable;
};
class NativeTrainingRuntime final : public TrainingRuntime {
   public:
    explicit NativeTrainingRuntime(NativeTrainingConfiguration);
    [[nodiscard]] contracts::ComputeTerminal Train(mmltk::backend::models::rfdetr::TrainRequest, std::stop_token,
                                                   const std::function<void(const services::TrainProcessProgress&)>&) override;
    [[nodiscard]] contracts::ProviderQueryResult Query(const contracts::ProviderPreferences&, std::stop_token) override;
    [[nodiscard]] contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&, contracts::ProviderOfferIdentity,
                                                         int, std::string_view, std::stop_token) override;
    [[nodiscard]] contracts::ProviderEffectResult Reconcile(const services::VastReconciliationRequest&, std::stop_token) override;

   private:
    NativeTrainingConfiguration config_;
};
enum class TrainingActivity : std::uint8_t {
    Idle,
    Local,
    ProviderQuery,
    Remote,
};
MMLTK_REFLECT_ENUM(TrainingActivity)
struct TrainingSnapshot final {
    std::uint64_t revision = 0U;
    TrainingActivity activity = TrainingActivity::Idle;
    contracts::ComputeUiState local{};
    contracts::ProviderOfferState offers{};
    contracts::RemoteSessionState remote{};
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::filesystem::path output_directory;
    std::optional<mmltk::backend::models::rfdetr::TrainingRecord> metrics;
    mmltk::backend::models::rfdetr::TrainingPersistence persistence{};
    mmltk::backend::models::rfdetr::TrainingCheckpointInspection inspection;
    bool operator==(const TrainingSnapshot&) const = default;
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Transient}]] TrainingProgress final {
    std::uint64_t revision = 0U;
    TrainingActivity activity = TrainingActivity::Idle;
    contracts::ComputeUiState local{};
    std::optional<mmltk::backend::models::rfdetr::TrainingRecord> metrics;
    mmltk::backend::models::rfdetr::TrainingPersistence persistence{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] TrainingChanged final {
    TrainingSnapshot snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] TrainingInspectionChanged final {
    mmltk::backend::models::rfdetr::TrainingCheckpointInspection inspection;
};
MMLTK_REFLECT_FIELDS(TrainingInspectionChanged)
class TrainingSystem final {
   public:
    using event_type = std::variant<TrainingProgress, TrainingChanged, TrainingInspectionChanged>;
    using RuntimeFactory = std::function<std::unique_ptr<TrainingRuntime>()>;
    TrainingSystem(SettingsSystem&, DatasetSystem&, ModelSystem&, std::optional<mmltk::common::system::ExecutionPolicyRequest>, RuntimeFactory,
                   SystemEventSink<event_type> = {});
    ~TrainingSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] TrainingSnapshot Start(contracts::WorkflowIntent<contracts::FeatureId::Train>);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::backend::models::rfdetr::TrainingOpenedRun OpenRun(
        mmltk::backend::models::rfdetr::TrainingDirectoryQuery);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::backend::models::rfdetr::TrainingHistoryPage History(
        mmltk::backend::models::rfdetr::TrainingHistoryQuery);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::backend::models::rfdetr::TrainingCheckpointInspection InspectCheckpoint(
        mmltk::backend::models::rfdetr::TrainingCheckpointQuery);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::backend::models::rfdetr::TrainingCheckpointInspection CancelCheckpointInspection();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] mmltk::backend::models::rfdetr::TrainingCheckpointCapability PrepareResume(
        mmltk::backend::models::rfdetr::TrainingCheckpointQuery);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] TrainingSnapshot Resume(mmltk::backend::models::rfdetr::TrainingCheckpointQuery);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] TrainingSnapshot Stop(contracts::WorkflowIntent<contracts::FeatureId::Train>) noexcept;
    void Shutdown() noexcept;
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] TrainingSnapshot Query(contracts::ProviderQueryIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] TrainingSnapshot Select(contracts::ProviderOfferIdentity);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] TrainingSnapshot Clear(
        // CLEANUP-IGNORE: Provider controls remain explicit reflected intent endpoints with distinct request types.
        contracts::ProviderClearIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] TrainingSnapshot StartRemote(contracts::ProviderStartIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] TrainingSnapshot StopRemote(contracts::ProviderStopIntent);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] TrainingSnapshot RetryReconciliation();
    [[= contracts::reflection::Snapshot{128U * 1024U}]] [[nodiscard]] TrainingSnapshot snapshot() const;

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
MMLTK_REFLECT_FIELDS(TrainingSnapshot)
MMLTK_REFLECT_FIELDS(TrainingProgress)
MMLTK_REFLECT_FIELDS(TrainingChanged)
}  // namespace mmltk::controller
