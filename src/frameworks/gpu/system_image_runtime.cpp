#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/image_product_retirement.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/common/system/execution_policy.h"
#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"
#include "src/common/types/generation.h"
#include <stdexcept>
#include <utility>

namespace mmltk::frameworks::gpu {
ImageProductRevisionSequence::ImageProductRevisionSequence(std::uint64_t next) : next_(next) {
    if (next == 0U) throw std::invalid_argument("image product revision sequence is zero");
}
std::uint64_t ImageProductRevisionSequence::Take() {
    std::scoped_lock lock(mutex_);
    return mmltk::common::types::take_monotonic_identity(next_);
}
namespace {

[[nodiscard]] std::exception_ptr missing_retention_failure() noexcept {
    try {
        throw std::runtime_error("system image model retained physical resources without a failure");
    } catch (...) { return std::current_exception(); }
}

class UnsafeRuntimeConstruction final : public std::exception {
   public:
    explicit UnsafeRuntimeConstruction(SystemImageRuntime::UnsafeCustody custody) noexcept : custody_(std::move(custody)) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "system image runtime construction retained unsafe physical resources";
    }
    [[nodiscard]] SystemImageRuntime::UnsafeCustody TakeCustody() noexcept { return std::move(custody_); }

   private:
    SystemImageRuntime::UnsafeCustody custody_;
};

}  // namespace

struct SystemImageRuntime::State final {
    explicit State(std::unique_ptr<SystemImageModel> adopted_model) noexcept : model(std::move(adopted_model)) {}

    void Finish(SystemImageRuntimeConfig& config, const std::shared_ptr<ImageProductRetirement>& products) {
        auto backend = config.backend ? std::move(config.backend) : cuda_image_copy_backend();
        if (config.adopted_context) {
            config.adopted_context->ValidateSelection(config.device, backend, config.context_mode, config.numa_node, config.execution);
            context = std::move(config.adopted_context);
        } else {
            context.emplace(config.device, std::move(backend), config.context_mode,
                            config.numa_node, std::move(config.execution));
        }
        std::optional<mmltk::common::system::ScopedExecutionPolicy> policy;
        if (const auto* execution = context->execution())
            policy.emplace(mmltk::common::system::ExecutionPolicyRequest{
                execution->placement.cpus, {}, 0, execution->placement.numa_node, -10, false});
        input = std::make_unique<ImageProductBuffer>(*context, config.input_layout);
        output = std::make_unique<ImageProductPool>(*context, config.output_layout, config.output_buffer_count, products);
        stream = std::make_unique<ImageStream>(*context);
        workspace_finalize = std::move(config.workspace_finalize);
    }

    std::optional<DeviceContext> context;
    std::unique_ptr<SystemImageModel> model;
    std::unique_ptr<ImageProductBuffer> input;
    std::unique_ptr<ImageProductPool> output;
    std::unique_ptr<ImageStream> stream;
    ImageWorkspaceFinalize workspace_finalize;
    bool retired = false;
};

struct SystemImageRuntime::RetentionControl final {
    RetentionControl() : terminal(1U), lease(ReserveTerminalCudaLease(terminal)) {}
    ~RetentionControl() noexcept {
        if (state && deferred && !products->unsafe()) {
            try {
                if (state->context) state->context->Bind();
            } catch (...) {
                deferred = false;
                failure = combine_image_failures(failure, std::current_exception());
            }
        }
        if (state && (!deferred || products->unsafe()))
            std::move(lease).Install(TerminalCudaCustody::Share(std::move(state)), cudaErrorUnknown);
    }

    TerminalCudaRetirementOwner terminal;
    TerminalCudaRetirementLease lease;
    const std::shared_ptr<ImageProductRetirement> products = std::make_shared<ImageProductRetirement>();
    std::shared_ptr<State> state;
    std::exception_ptr failure{};
    bool deferred = false;
};

std::shared_ptr<SystemImageRuntime::RetentionControl> SystemImageRuntime::ReserveRetention() {
    return std::make_shared<RetentionControl>();
}

SystemImageRuntime::UnsafeCustody::UnsafeCustody(std::shared_ptr<RetentionControl> control) noexcept : control_(std::move(control)) {}

bool SystemImageRuntime::UnsafeCustody::valid() const noexcept { return control_ && control_->state; }

std::exception_ptr SystemImageRuntime::UnsafeCustody::failure() const noexcept {
    if (!control_) return {};
    return combine_image_failures(control_->failure, control_->state ? control_->products->failure() : std::exception_ptr{});
}
bool SystemImageRuntime::UnsafeCustody::deferred() const noexcept { return valid() && control_->deferred; }
ImageStreamSettlement SystemImageRuntime::UnsafeCustody::FinishRetirement() noexcept {
    if (!valid()) return {.completion_reached = true};
    if (!control_->deferred) return {.failure = failure()};
    const auto settled = control_->products->Retire();
    auto reported = combine_image_failures(control_->failure, settled.failure);
    if (control_->products->unsafe()) {
        control_->deferred = false;
        control_->products->SetRetirementSink({});
    }
    if (settled.completion_reached) {
        try {
            if (control_->state->context) control_->state->context->Bind();
        } catch (...) {
            control_->deferred = false;
            control_->failure = combine_image_failures(reported, std::current_exception());
            control_->products->SetRetirementSink({});
            return {.failure = failure()};
        }
        control_->products->SetRetirementSink({});
        control_->state.reset();
    }
    return {.completion_reached = settled.completion_reached, .failure = reported};
}
void SystemImageRuntime::UnsafeCustody::SetRetirementSink(std::shared_ptr<const std::function<void()>> sink) noexcept {
    if (!valid()) return;
    control_->products->SetRetirementSink(std::move(sink));
}

std::optional<SystemImageRuntime::UnsafeCustody> SystemImageRuntime::UnsafeConstruction(const std::exception_ptr failure) noexcept {
    const auto construction = find_image_failure<UnsafeRuntimeConstruction>(failure);
    if (!construction) return std::nullopt;
    try {
        std::rethrow_exception(construction);
    } catch (UnsafeRuntimeConstruction& retained) {
        auto custody = retained.TakeCustody();
        if (custody.valid() && failure != construction) custody.control_->failure = combine_image_failures(custody.failure(), failure);
        if (custody.valid()) return std::optional<UnsafeCustody>{std::move(custody)};
        return std::nullopt;
    } catch (...) { return std::nullopt; }
}

SystemImageRuntime::SystemImageRuntime(SystemImageRuntimeConfig config)
    : retention_(ReserveRetention()), product_revision_sequence_(std::move(config.product_revisions)) {
    state_ = std::make_shared<State>(std::move(config.model));
    try {
        if (!product_revision_sequence_) throw std::invalid_argument("image product revision sequence is unavailable");
        state_->Finish(config, retention_->products);
    } catch (...) {
        const auto construction_failure = std::current_exception();
        if (!state_->context && state_->model) {
            state_->model->StopIngress();
            throw UnsafeRuntimeConstruction{Retain(construction_failure)};
        }
        auto retirement = Retire();
        const auto failure = combine_image_failures(construction_failure, retirement.failure);
        if (retirement.custody.valid()) retirement.custody.control_->failure = failure;
        if (!retirement.safe_to_destroy) { throw UnsafeRuntimeConstruction{std::move(retirement.custody)}; }
        state_.reset();
        retention_.reset();
        std::rethrow_exception(failure);
    }
}
SystemImageRuntime::~SystemImageRuntime() noexcept {
    auto retirement = Retire();
    if (retirement.safe_to_destroy) state_.reset();
    retention_.reset();
}
bool SystemImageRuntime::UsesContext(const DeviceContext& context) const noexcept {
    return state_ && !state_->retired && state_->context && *state_->context == context;
}
int SystemImageRuntime::device() const noexcept { return state_ && !state_->retired && state_->context ? state_->context->device() : -1; }
const DeviceExecution* SystemImageRuntime::execution() const noexcept {
    return state_ && state_->context ? state_->context->execution() : nullptr;
}
void SystemImageRuntime::BindContext() {
    auto& state = ActiveState();
    if (!state.context) throw std::runtime_error("system image runtime context is unavailable");
    state.context->Bind();
}
void SystemImageRuntime::BeginWork() { BindContext(); }
SystemImageRuntime::Retirement SystemImageRuntime::Retire(std::exception_ptr unproved_execution) noexcept {
    if (!state_ && retention_ && retention_->state)
        return {.failure = UnsafeCustody{retention_}.failure(), .custody = UnsafeCustody{retention_}};
    if (!state_ || state_->retired) return {.safe_to_destroy = true};
    if (unproved_execution) return {.failure = unproved_execution, .custody = Retain(std::move(unproved_execution))};
    if (state_->model) state_->model->StopIngress();
    if (!state_->context) {
        if (!state_->model) {
            state_->retired = true;
            return {.safe_to_destroy = true};
        }
        const auto failure = missing_retention_failure();
        return {.failure = failure, .custody = Retain(failure)};
    }
    auto settled = state_->stream ? state_->stream->Settle() : ImageCopyBackend::StreamSettlement{.completion_reached = true};
    if (state_->output) {
        const auto display = state_->output->SettleWorkspaces();
        settled.completion_reached = settled.completion_reached && display.completion_reached;
        settled.failure = combine_image_failures(settled.failure, display.failure);
    }
    if (const auto display_failure = retention_->products->failure()) {
        settled.failure = combine_image_failures(settled.failure, display_failure);
    }
    settled.completion_reached = settled.completion_reached && !retention_->products->unsafe();
    if (!settled.completion_reached) {
        const auto failure = settled.failure ? settled.failure : missing_retention_failure();
        return {.failure = failure, .custody = Retain(failure)};
    }
    std::exception_ptr failure = settled.failure;
    try {
        state_->context->Bind();
    } catch (...) {
        failure = combine_image_failures(failure, std::current_exception());
        return {.failure = failure, .custody = Retain(failure)};
    }
    if (state_->model) {
        const auto released = state_->model->ReleaseResources();
        failure = combine_image_failures(failure, released.failure);
        if (!released.all_released) {
            if (!failure) failure = missing_retention_failure();
            return {.failure = failure, .custody = Retain(failure)};
        }
        try {
            state_->context->Bind();
        } catch (...) {
            failure = combine_image_failures(failure, std::current_exception());
            return {.failure = failure, .custody = Retain(failure)};
        }
        state_->model.reset();
    }
    // Release ordinary pool ownership while the runtime can still observe
    // product cleanup. External products, raw aliases and counted reads keep
    // their own exact storage; healthy delayed release is a deferred outcome.
    if (state_->output) {
        state_->output->ReleaseForRetirement();
        state_->output.reset();
    }
    const auto products = retention_->products->Retire();
    failure = combine_image_failures(failure, products.failure);
    if (!products.completion_reached) return {.failure = failure, .custody = Retain(failure, !retention_->products->unsafe())};
    state_->retired = true;
    return {.safe_to_destroy = true, .failure = failure};
}
SystemImageRuntime::UnsafeCustody SystemImageRuntime::Retain(std::exception_ptr failure, const bool deferred) noexcept {
    static_cast<void>(retention_->products->Retire());
    if (!failure && !deferred) failure = missing_retention_failure();
    retention_->failure = std::move(failure);
    retention_->deferred = deferred;
    retention_->state = std::move(state_);
    return UnsafeCustody{retention_};
}
SystemImageRuntime::State& SystemImageRuntime::ActiveState() {
    if (!state_ || state_->retired) throw std::runtime_error("system image runtime is retired");
    return *state_;
}
const SystemImageRuntime::State& SystemImageRuntime::ActiveState() const {
    if (!state_ || state_->retired) throw std::runtime_error("system image runtime is retired");
    return *state_;
}
SystemImageRuntime::CompletedOutput SystemImageRuntime::Completed() const { return ActiveState().output->Selected(); }
ImageProductPool::Availability SystemImageRuntime::ObserveOutputAvailability() const { return ActiveState().output->ObserveAvailability(); }
ImageProductPool::Facts SystemImageRuntime::OutputFacts() const { return ActiveState().output->SelectedFacts(); }
ImageStorageFootprint SystemImageRuntime::OutputStorageFootprint() const { return ActiveState().output->StorageFootprint(); }
SystemImageRuntime::OutputCandidate SystemImageRuntime::AcquireOutput(const std::stop_token stop, CompletedOutput baseline,
                                                                      ImagePlanePreservation preservation) {
    return ActiveState().output->Acquire(stop, std::move(baseline), preservation);
}
SystemImageRuntime::OutputCandidate SystemImageRuntime::TryAcquireOutput(CompletedOutput& baseline, ImagePlanePreservation preservation) {
    // CLEANUP-IGNORE: Acquisition, clearing publication and retained publication are distinct pool operations exposed by the runtime.
    return ActiveState().output->TryAcquire(baseline, preservation);
}
void SystemImageRuntime::Publish(OutputCandidate& candidate, const std::uint32_t width, const std::uint32_t height,
                                 ImageProductBuffer::ProductSubmit submit) {
    auto& state = ActiveState();
    state.output->Publish(*state.stream, candidate, width, height, TakeProductRevision(), std::move(submit));
}
SystemImageRuntime::CompletedOutput SystemImageRuntime::CommitOutput(OutputCandidate&& candidate) {
    ActiveState().output->FinalizeWorkspace(candidate, {});
    return ActiveState().output->Commit(std::move(candidate));
}
void SystemImageRuntime::PublishRetained(OutputCandidate& candidate, const std::uint32_t width, const std::uint32_t height,
                                         ImageProductBuffer::ProductSubmit submit, ImageSubmission submission) {
    auto& state = ActiveState();
    state.output->PublishRetained(*state.stream, candidate, width, height, TakeProductRevision(), std::move(submit), submission);
}
void SystemImageRuntime::NotifyWorkCompletion(std::function<void()> wake) { ActiveState().stream->Notify(std::move(wake)); }
void SystemImageRuntime::CompleteWork() { ActiveState().stream->Synchronize(); }
void SystemImageRuntime::FinalizeWorkspace(OutputCandidate& candidate, ImageWorkspaceCoverage coverage) {
    ActiveState().output->FinalizeWorkspace(candidate, coverage);
}
void SystemImageRuntime::CompleteWorkspaces() { ActiveState().output->CompleteWorkspaces(); }
bool SystemImageRuntime::DetachDisplay(const std::shared_ptr<ImageWorkspace>& workspace) {
    auto& state = ActiveState();
    return state.output->DetachDisplay(*state.stream, workspace);
}
bool SystemImageRuntime::PrepareDisplay(std::uint64_t revision, const std::shared_ptr<ImageWorkspace>& workspace) {
    auto& state = ActiveState();
    if (!workspace || !workspace->admitted() || workspace->retired())
        throw std::invalid_argument("shared display workspace is unavailable");
    return state.output->PrepareDisplay(*state.stream, revision, workspace, state.workspace_finalize);
}
BorrowedImageWorkspace SystemImageRuntime::BorrowWorkspace() const { return ActiveState().output->BorrowWorkspace(); }
ImageWorkspaceObservation SystemImageRuntime::ObserveWorkspace() const { return ActiveState().output->ObserveWorkspace(); }
void SystemImageRuntime::SelectOutput(const CompletedOutput& product) { ActiveState().output->Select(product); }
void SystemImageRuntime::SetOutputAvailableSink(std::function<void()> sink) { ActiveState().output->SetAvailabilitySink(std::move(sink)); }
BorrowedImageProductReadView SystemImageRuntime::BorrowInput() const { return ActiveState().input->Borrow(); }
void SystemImageRuntime::PublishInput(const std::uint32_t width, const std::uint32_t height, ImageProductBuffer::ProductSubmit submit) {
    auto& state = ActiveState();
    try {
        state.input->Publish(*state.stream, width, height, std::move(submit));
        state.stream->Synchronize();
    } catch (...) { state.stream->RethrowAfterSettlement(std::current_exception()); }
}
BorrowedImageProductReadView SystemImageRuntime::Borrow() const { return ActiveState().output->Borrow(); }
SystemImageModel* SystemImageRuntime::model() noexcept { return state_ && !state_->retired ? state_->model.get() : nullptr; }
std::array<ImageCopyPath, 2U> SystemImageRuntime::CopyFrom(OutputCandidate& candidate, BorrowedImageProductReadView source) {
    auto& state = ActiveState();
    return state.output->CopyFrom(*state.stream, candidate, std::move(source), TakeProductRevision());
}
std::array<ImageCopyPath, 2U> SystemImageRuntime::CopyFrom(BorrowedImageProductReadView source) {
    auto& state = ActiveState();
    return state.output->CopyFrom(*state.stream, std::move(source), TakeProductRevision());
}
std::array<ImageCopyPath, 2U> SystemImageRuntime::CopyInputFrom(BorrowedImageProductReadView source,
                                                                ImageProductBuffer::MissingPlaneSubmit initialize_missing,
                                                                const bool preserve_clean) {
    auto& state = ActiveState();
    return state.input->CopyFrom(*state.stream, std::move(source), std::move(initialize_missing), preserve_clean);
}
void SystemImageRuntime::Publish(const std::uint32_t width, const std::uint32_t height, ImageProductBuffer::ProductSubmit submit) {
    auto& state = ActiveState();
    auto candidate = state.output->Acquire();
    if (!candidate.valid()) throw std::runtime_error("image output candidate is unavailable");
    state.output->Publish(*state.stream, candidate, width, height, TakeProductRevision(), std::move(submit));
    static_cast<void>(CommitOutput(std::move(candidate)));
}
std::uint64_t SystemImageRuntime::TakeProductRevision() { return product_revision_sequence_->Take(); }
}  // namespace mmltk::frameworks::gpu
