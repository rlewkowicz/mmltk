#include "src/controller/shell/direct_visual_systems.h"

#include <array>
#include <algorithm>
#include <stdexcept>
#include <utility>

#include "src/controller/browser/application_materializer.h"
#include "src/controller/browser/application_event_publisher.h"
#include "src/controller/presentation/visual_document.h"
#include "src/controller/presentation/visual_runtime.h"
#include "src/controller/subsystems/system/compute_systems.h"
#include "src/controller/subsystems/system/predict_system.h"
#include "src/frameworks/gpu/system_image_runtime.h"

namespace mmltk::controller::shell {

SystemEventSink<ExploreSystem::event_type> make_explore_upscale_event_sink(ApplicationSystemStorage::EventSink& sink,
                                                                           UpscaleSystem& upscale,
                                                                           ApplicationSystemStorage::ContinuitySink continuity,
                                                                           std::function<void(PresentationSourceIdentity)> source) {
    return [&upscale, publisher = browser::ApplicationEventPublisher<&ApplicationSystems::explore>(
                          sink, std::move(continuity), std::move(source))](ExploreSystem::event_type event) noexcept {
        if (const auto* changed = std::get_if<ExploreChanged>(&event);
            changed != nullptr && changed->snapshot.ready && !changed->snapshot.busy &&
            (changed->snapshot.mode != ExploreMode::Gallery ||
             std::ranges::all_of(changed->snapshot.gallery.slots, [](const bool ready) { return ready; })))
            upscale.Warm({changed->snapshot.dataset.image_width, changed->snapshot.dataset.image_height});
        publisher(event);
    };
}

std::unique_ptr<ExploreSystem> make_shell_explore_system(SettingsSystem& settings, const ApplicationSystemConfiguration& configuration,
                                                         const mmltk::frameworks::gpu::DeviceExecution& execution,
                                                         SystemEventSink<ExploreSystem::event_type> events,
                                                         const VisualDiagnosticSink diagnostics) {
    const auto nproc = normalize_explore_parallelism(configuration.explore_nproc, execution.placement);
    return std::make_unique<ExploreSystem>(
        settings, configuration.base_visual, nproc,
        [&settings, visual = configuration.base_visual, native = configuration.explore, nproc](auto revisions) mutable {
            const auto selected = settings.explore_settings_candidate();
            visual.device = selected.device_id;
            visual.numa_node = selected.loading.numa_node;
            native.loading = selected.loading;
            return make_native_explore_runtime_factory(visual, nproc, native)(std::move(revisions));
        },
        std::move(events), diagnostics);
}

ApplicationSystemStorage::ApplicationSystemStorage(ApplicationSystemConfiguration configuration, EventSink events,
                                                   const VisualDiagnosticSink diagnostics, ContinuitySink continuity)
    : events_(std::move(events)), continuity_(std::move(continuity)) {
    const auto borrow_exact = [this](const VisualFrame& frame) { return BorrowDocument(frame); };
    const auto source_changed = [this](const PresentationSourceIdentity source) {
        if (auto* presentation = presentation_notifications_.load(std::memory_order_acquire)) presentation->SourceChanged(source);
    };
    settings_ = std::make_unique<SettingsSystem>([this, publisher = browser::ApplicationEventPublisher<&ApplicationSystems::settings>(
                                                            events_, continuity_)](SettingsSystem::event_type event) noexcept {
        publisher(event);
        if (explore_) explore_->ExecutionSettingsChanged();
    });
    const auto initial_settings = settings_->Load(configuration.settings_location, configuration.h2d_dataloader);
    if (!initial_settings.applied())
        throw std::runtime_error(initial_settings.detail.empty() ? "initial settings load was not applied" : initial_settings.detail);
    file_dialog_ = std::make_unique<FileDialogSystem>(
        [client = configuration.file_dialog, settings = settings_.get()] {
            return std::make_unique<NativeFileDialogRuntime>(client, *settings);
        },
        browser::ApplicationEventPublisher<&ApplicationSystems::file_dialog>(events_, continuity_));
    dataset_ = std::make_unique<DatasetSystem>(
        *settings_, [] { return std::make_unique<ArtifactDatasetRuntime>(); },
        browser::ApplicationEventPublisher<&ApplicationSystems::dataset>(events_, continuity_));
    model_ = std::make_unique<ModelSystem>(
        *settings_, [] { return std::make_unique<ArtifactModelRuntime>(); },
        browser::ApplicationEventPublisher<&ApplicationSystems::model>(events_, continuity_));
    training_ = std::make_unique<TrainingSystem>(
        *settings_, *dataset_, *model_,
        [provider = configuration.provider, executable = std::move(configuration.training_executable)] {
            return std::make_unique<NativeTrainingRuntime>(NativeTrainingConfiguration{
                .provider = provider,
                .training_executable = executable,
            });
        },
        browser::ApplicationEventPublisher<&ApplicationSystems::training>(events_, continuity_));
    const DirectComputeConfiguration compute{
        .execution = resolve_visual_device_execution(configuration.base_visual),
    };
    validation_ = std::make_unique<ValidationSystem>(
        *settings_, *dataset_, *model_, [compute] { return std::make_unique<CudaValidationRuntime>(compute); },
        browser::ApplicationEventPublisher<&ApplicationSystems::validation>(events_, continuity_, source_changed), compute.execution, configuration.base_visual);
    export_ = std::make_unique<ExportSystem>(
        *settings_, *dataset_, *model_, [compute] { return std::make_unique<CudaExportRuntime>(compute); },
        browser::ApplicationEventPublisher<&ApplicationSystems::export_system>(events_, continuity_), compute.execution);
    predict_ = std::make_unique<PredictSystem>(
        *settings_, *dataset_, *model_, configuration.base_visual, [compute] { return std::make_unique<CudaPredictRuntime>(compute); },
        browser::ApplicationEventPublisher<&ApplicationSystems::predict>(events_, continuity_, source_changed));
    upscale_ = std::make_unique<UpscaleSystem>(
        configuration.output_visual, make_native_upscale_runtime_factory(configuration.output_visual), borrow_exact,
        browser::ApplicationEventPublisher<&ApplicationSystems::upscale>(events_, continuity_, source_changed), diagnostics);
    explore_ = make_shell_explore_system(*settings_, configuration, *compute.execution,
                                         make_explore_upscale_event_sink(events_, *upscale_, continuity_, source_changed), diagnostics);
    annotation_ = std::make_unique<AnnotationSystem>(
        configuration.output_visual, make_native_annotation_runtime_factory(configuration.output_visual), borrow_exact,
        browser::ApplicationEventPublisher<&ApplicationSystems::annotation>(events_, continuity_, source_changed), diagnostics);
    live_ = std::make_unique<LiveSystem>(
        configuration.base_visual, make_native_live_runtime_factory(configuration.base_visual, std::move(configuration.live)),
        browser::ApplicationEventPublisher<&ApplicationSystems::live>(events_, continuity_, source_changed), diagnostics);

    systems_.settings = settings_.get();
    systems_.file_dialog = file_dialog_.get();
    systems_.dataset = dataset_.get();
    systems_.model = model_.get();
    systems_.training = training_.get();
    systems_.validation = validation_.get();
    systems_.export_system = export_.get();
    systems_.predict = predict_.get();
    systems_.explore = explore_.get();
    systems_.annotation = annotation_.get();
    systems_.upscale = upscale_.get();
    systems_.live = live_.get();
    source_readers_ = browser::materialize_visual_source_readers(systems_);
    presentation_ = std::make_unique<PresentationSystem>(
        configuration.output_visual,
        make_native_presentation_writer_factory(configuration.output_visual, std::move(configuration.presentation), diagnostics),
        source_readers_, browser::ApplicationEventPublisher<&ApplicationSystems::presentation>(events_, continuity_), diagnostics);
    systems_.presentation = presentation_.get();
    presentation_notifications_.store(presentation_.get(), std::memory_order_release);
}

mmltk::frameworks::gpu::BorrowedImageProductReadView ApplicationSystemStorage::BorrowExactFrame(const VisualFrame& frame) const {
    if (!frame.valid()) return {};
    const auto found = std::ranges::find(source_readers_, frame.source, &VisualSourceReader::source);
    if (found == source_readers_.end()) return {};
    return borrow_matching_visual_product(frame, found->borrow());
}

ApplicationSystemStorage::~ApplicationSystemStorage() { presentation_notifications_.store(nullptr, std::memory_order_release); }
VisualDocumentRead ApplicationSystemStorage::BorrowDocument(const VisualFrame& frame) const {
    if (frame.source.kind == PresentationSourceKind::Explore) return explore_->BorrowDocument(frame);
    if (frame.source.kind == PresentationSourceKind::Upscale) return upscale_->BorrowDocument(frame);
    auto pixels = BorrowExactFrame(frame);
    if (!pixels.valid()) return {};
    auto document = std::make_shared<VisualDocument>();
    document->scene.document = contracts::WorkspaceResource::From("direct://annotation", frame.revision);
    document->scene.categories.push_back({.value = "object"});
    return {std::move(pixels), std::move(document)};
}

ApplicationSystems& ApplicationSystemStorage::application_systems() noexcept { return systems_; }

PresentationSystem& ApplicationSystemStorage::presentation() noexcept { return *presentation_; }

FileDialogSystem& ApplicationSystemStorage::file_dialog() noexcept { return *file_dialog_; }
DatasetSystem& ApplicationSystemStorage::dataset() noexcept { return *dataset_; }
ModelSystem& ApplicationSystemStorage::model() noexcept { return *model_; }
TrainingSystem& ApplicationSystemStorage::training() noexcept { return *training_; }
ValidationSystem& ApplicationSystemStorage::validation() noexcept { return *validation_; }
ExportSystem& ApplicationSystemStorage::export_system() noexcept {
    // CLEANUP-IGNORE: Each typed system accessor preserves the sealed ApplicationSystemStorage boundary.
    return *export_;
}
PredictSystem& ApplicationSystemStorage::predict() noexcept { return *predict_; }
ExploreSystem& ApplicationSystemStorage::explore() noexcept { return *explore_; }
AnnotationSystem& ApplicationSystemStorage::annotation() noexcept { return *annotation_; }
UpscaleSystem& ApplicationSystemStorage::upscale() noexcept { return *upscale_; }
LiveSystem& ApplicationSystemStorage::live() noexcept { return *live_; }

}  // namespace mmltk::controller::shell
