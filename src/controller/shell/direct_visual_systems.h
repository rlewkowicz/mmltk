#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

#include "src/controller/browser/client_record.h"
#include "src/controller/contracts/application_systems.h"

namespace mmltk::controller::shell {

struct ApplicationSystemConfiguration final {
    VisualDeviceSettings base_visual{};
    VisualDeviceSettings output_visual{};
    std::size_t explore_nproc = 0U;
    ExploreNativeConfiguration explore{};
    LiveNativeConfiguration live{};
    PresentationNativeConfiguration presentation{};
    services::SettingsLocation settings_location{std::string_view{}};
    bool h2d_dataloader = true;
    services::FileDialogClient file_dialog{};
    services::VastProviderClient provider{};
    std::filesystem::path training_executable{};
};

[[nodiscard]] std::unique_ptr<ExploreSystem> make_shell_explore_system(SettingsSystem&, const ApplicationSystemConfiguration&,
                                                                       const mmltk::frameworks::gpu::DeviceExecution&,
                                                                       SystemEventSink<ExploreSystem::event_type> = {},
                                                                       VisualDiagnosticSink = {});

class ApplicationSystemStorage final {
   public:
    using EventSink = std::function<void(browser::SystemEvent)>;

    ApplicationSystemStorage(ApplicationSystemConfiguration, EventSink, VisualDiagnosticSink = {});
    ~ApplicationSystemStorage();

    ApplicationSystemStorage(const ApplicationSystemStorage&) = delete;
    ApplicationSystemStorage& operator=(const ApplicationSystemStorage&) = delete;

    [[nodiscard]] ApplicationSystems& application_systems() noexcept;
    [[nodiscard]] PresentationSystem& presentation() noexcept;
    [[nodiscard]] FileDialogSystem& file_dialog() noexcept;
    [[nodiscard]] DatasetSystem& dataset() noexcept;
    [[nodiscard]] ModelSystem& model() noexcept;
    [[nodiscard]] TrainingSystem& training() noexcept;
    // CLEANUP-IGNORE: The sealed application owner exposes each independently typed system through a direct
    // accessor; a shared erased or templated route would weaken the ordinary API.
    [[nodiscard]] ValidationSystem& validation() noexcept;
    [[nodiscard]] ExportSystem& export_system() noexcept;
    [[nodiscard]] PredictSystem& predict() noexcept;
    [[nodiscard]] ExploreSystem& explore() noexcept;
    [[nodiscard]] AnnotationSystem& annotation() noexcept;
    [[nodiscard]] UpscaleSystem& upscale() noexcept;
    [[nodiscard]] LiveSystem& live() noexcept;

   private:
    [[nodiscard]] mmltk::frameworks::gpu::BorrowedImageProductReadView BorrowExactFrame(const VisualFrame&) const;
    [[nodiscard]] VisualDocumentRead BorrowDocument(const VisualFrame&) const;

    // CLEANUP-IGNORE: ApplicationSystemStorage explicitly owns its system instances; a transport channel's queues do
    // not.
    EventSink events_;
    std::unique_ptr<SettingsSystem> settings_;
    std::unique_ptr<FileDialogSystem> file_dialog_;
    std::unique_ptr<DatasetSystem> dataset_;
    std::unique_ptr<ModelSystem> model_;
    std::unique_ptr<TrainingSystem> training_;
    std::unique_ptr<ValidationSystem> validation_;
    std::unique_ptr<ExportSystem> export_;
    std::unique_ptr<PredictSystem> predict_;
    std::unique_ptr<UpscaleSystem> upscale_;
    std::unique_ptr<ExploreSystem> explore_;
    std::unique_ptr<AnnotationSystem> annotation_;
    std::unique_ptr<LiveSystem> live_;
    std::array<VisualSourceReader, 5U> source_readers_{};
    std::unique_ptr<PresentationSystem> presentation_;
    ApplicationSystems systems_{};
};

[[nodiscard]] SystemEventSink<ExploreSystem::event_type> make_explore_upscale_event_sink(ApplicationSystemStorage::EventSink&,
                                                                                         UpscaleSystem&);

}  // namespace mmltk::controller::shell
