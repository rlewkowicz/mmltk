#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/services/file_dialog_catalog.h"
#include "src/controller/services/file_dialog_client.h"
#include "src/controller/services/file_dialog_types.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/system/local_run.h"
#include "src/controller/subsystems/system/system_events.h"
namespace mmltk::controller {
struct FileDialogSnapshot final {
    std::uint64_t generation = 0U;
    bool active = false;
    bool cancellation_requested = false;
    services::FileDialogTarget target{};
    std::optional<services::FileDialogSelection> selection{};
    [[nodiscard]] bool valid() const noexcept {
        if (generation == 0U) return !active && !cancellation_requested && !services::valid_file_dialog_target(target) && !selection;
        if (!services::valid_file_dialog_target(target)) return false;
        if (cancellation_requested && !active) return false;
        return active ? !selection : !selection || selection->valid_for(target);
    }
    // CLEANUP-IGNORE: FileDialogSnapshot has its own reflected validity boundary despite sharing an event payload
    // shape.
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] FileDialogCompleted final {
    FileDialogSnapshot snapshot{};
};
struct[[= contracts::reflection::Event{contracts::reflection::EventDelivery::Critical}]] FileDialogFailed final {
    FileDialogSnapshot snapshot{};
    [[= mmltk::frameworks::reflection::MaxBytes{services::kFileDialogTextCapacity}]] std::string detail;
};
class FileDialogRuntime {
   public:
    virtual ~FileDialogRuntime() = default;
    [[nodiscard]] virtual services::FileDialogSelection Open(const services::ResolvedFileDialog&, std::stop_token) = 0;
};
class NativeFileDialogRuntime final : public FileDialogRuntime {
   public:
    NativeFileDialogRuntime(services::FileDialogClient client, SettingsSystem& settings) : client_(client), settings_(settings) {}
    [[nodiscard]] services::FileDialogSelection Open(const services::ResolvedFileDialog&, std::stop_token) override;

   private:
    services::FileDialogClient client_;
    SettingsSystem& settings_;
};
class FileDialogSystem final {
   public:
    using event_type = std::variant<FileDialogCompleted, FileDialogFailed>;
    using RuntimeFactory = std::function<std::unique_ptr<FileDialogRuntime>()>;
    FileDialogSystem(RuntimeFactory, SystemEventSink<event_type> = {});
    ~FileDialogSystem();
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] FileDialogSnapshot Open(services::FileDialogOpen);
    [[= contracts::reflection::direct::IntentEndpoint{}]] [[nodiscard]] FileDialogSnapshot Stop() noexcept;
    void Shutdown() noexcept;
    [[= contracts::reflection::Snapshot{8192U}]] [[nodiscard]] FileDialogSnapshot snapshot() const;

   private:
    RuntimeFactory factory_;
    SystemEventSink<event_type> events_;
    mutable std::mutex mutex_;
    FileDialogSnapshot state_{};
    std::unique_ptr<FileDialogRuntime> runtime_;
    direct::LocalRun run_;
};
MMLTK_REFLECT_FIELDS(FileDialogSnapshot)
MMLTK_REFLECT_FIELDS(FileDialogCompleted)
MMLTK_REFLECT_FIELDS(FileDialogFailed)
}  // namespace mmltk::controller
