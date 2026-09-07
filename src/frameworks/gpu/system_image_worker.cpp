#include "src/frameworks/gpu/system_image_worker.h"

#include <stdexcept>
#include <utility>
#include "src/common/system/execution_policy.h"

#include "src/frameworks/gpu/terminal_cuda_retirement_owner.h"

namespace mmltk::frameworks::gpu {
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

    void Finish(SystemImageRuntimeConfig& config) {
        context.emplace(config.device, config.backend ? std::move(config.backend) : cuda_image_copy_backend(), config.context_mode,
                        config.numa_node, std::move(config.execution));
        std::optional<mmltk::common::system::ScopedExecutionPolicy> policy;
        if (const auto* execution = context->execution())
            policy.emplace(mmltk::common::system::ExecutionPolicyRequest{
                execution->placement.cpus, {}, 0, execution->placement.numa_node, -10, false});
        input = std::make_unique<ImageProductBuffer>(*context, config.input_layout);
        output = std::make_unique<ImageProductBuffer>(*context, config.output_layout);
        stream = std::make_unique<ImageStream>(*context);
    }

    std::optional<DeviceContext> context;
    std::unique_ptr<SystemImageModel> model;
    std::unique_ptr<ImageProductBuffer> input;
    std::unique_ptr<ImageProductBuffer> output;
    std::unique_ptr<ImageStream> stream;
    bool retired = false;
};

struct SystemImageRuntime::RetentionControl final {
    RetentionControl() : terminal(1U), lease(ReserveTerminalCudaLease(terminal)) {}
    ~RetentionControl() noexcept {
        if (state) std::move(lease).Install(TerminalCudaCustody::Share(std::move(state)), cudaErrorUnknown);
    }

    TerminalCudaRetirementOwner terminal;
    TerminalCudaRetirementLease lease;
    std::shared_ptr<State> state;
    std::exception_ptr failure{};
};

std::shared_ptr<SystemImageRuntime::RetentionControl> SystemImageRuntime::ReserveRetention() {
    return std::make_shared<RetentionControl>();
}

SystemImageRuntime::UnsafeCustody::UnsafeCustody(std::shared_ptr<RetentionControl> control) noexcept : control_(std::move(control)) {}

bool SystemImageRuntime::UnsafeCustody::valid() const noexcept { return control_ && control_->state; }

std::exception_ptr SystemImageRuntime::UnsafeCustody::failure() const noexcept {
    return control_ ? control_->failure : std::exception_ptr{};
}

std::optional<SystemImageRuntime::UnsafeCustody> SystemImageRuntime::UnsafeConstruction(const std::exception_ptr failure) noexcept {
    if (!failure) return std::nullopt;
    try {
        std::rethrow_exception(failure);
    } catch (UnsafeRuntimeConstruction& retained) {
        auto custody = retained.TakeCustody();
        if (custody.valid()) return std::optional<UnsafeCustody>{std::move(custody)};
        return std::nullopt;
    } catch (...) { return std::nullopt; }
}

SystemImageRuntime::SystemImageRuntime(SystemImageRuntimeConfig config) : retention_(ReserveRetention()) {
    state_ = std::make_shared<State>(std::move(config.model));
    try {
        state_->Finish(config);
    } catch (...) {
        const auto construction_failure = std::current_exception();
        if (!state_->context && state_->model) {
            state_->model->StopIngress();
            throw UnsafeRuntimeConstruction{Retain(construction_failure)};
        }
        auto retirement = Retire();
        if (!retirement.safe_to_destroy) { throw UnsafeRuntimeConstruction{std::move(retirement.custody)}; }
        state_.reset();
        retention_.reset();
        std::rethrow_exception(construction_failure);
    }
}
SystemImageRuntime::~SystemImageRuntime() noexcept {
    auto retirement = Retire();
    if (retirement.safe_to_destroy) state_.reset();
    retention_.reset();
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
SystemImageRuntime::Retirement SystemImageRuntime::Retire() noexcept {
    if (!state_ || state_->retired) return {.safe_to_destroy = true};
    if (state_->model) state_->model->StopIngress();
    if (!state_->context) {
        if (!state_->model) {
            state_->retired = true;
            return {.safe_to_destroy = true};
        }
        const auto failure = missing_retention_failure();
        return {.failure = failure, .custody = Retain(failure)};
    }
    const auto settled = state_->stream ? state_->stream->Settle() : ImageCopyBackend::StreamSettlement{.completion_reached = true};
    if (!settled.completion_reached) {
        const auto failure = settled.failure ? settled.failure : missing_retention_failure();
        return {.failure = failure, .custody = Retain(failure)};
    }
    std::exception_ptr failure = settled.failure;
    try {
        state_->context->Bind();
    } catch (...) {
        failure = failure ? failure : std::current_exception();
        return {.failure = failure, .custody = Retain(failure)};
    }
    if (state_->model) {
        const auto released = state_->model->ReleaseResources();
        if (released.failure && !failure) failure = released.failure;
        if (!released.all_released) {
            failure = failure ? failure : (released.failure ? released.failure : missing_retention_failure());
            return {.failure = failure, .custody = Retain(failure)};
        }
        try {
            state_->context->Bind();
        } catch (...) {
            failure = failure ? failure : std::current_exception();
            return {.failure = failure, .custody = Retain(failure)};
        }
        state_->model.reset();
    }
    state_->retired = true;
    return {.safe_to_destroy = true, .failure = failure};
}
SystemImageRuntime::UnsafeCustody SystemImageRuntime::Retain(std::exception_ptr failure) noexcept {
    if (!failure) failure = missing_retention_failure();
    retention_->failure = std::move(failure);
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
ImageProductBuffer& SystemImageRuntime::output() { return *ActiveState().output; }
const ImageProductBuffer& SystemImageRuntime::output() const { return *ActiveState().output; }
BorrowedImageProductReadView SystemImageRuntime::BorrowInput() const { return ActiveState().input->Borrow(); }
BorrowedImageProductReadView SystemImageRuntime::Borrow() const { return ActiveState().output->Borrow(); }
SystemImageModel* SystemImageRuntime::model() noexcept { return state_ && !state_->retired ? state_->model.get() : nullptr; }
std::array<ImageCopyPath, 2U> SystemImageRuntime::CopyFrom(BorrowedImageProductReadView source) {
    auto& state = ActiveState();
    return state.output->CopyFrom(*state.stream, std::move(source));
}
std::array<ImageCopyPath, 2U> SystemImageRuntime::CopyInputFrom(BorrowedImageProductReadView source,
                                                                ImageProductBuffer::MissingPlaneSubmit initialize_missing) {
    auto& state = ActiveState();
    return state.input->CopyFrom(*state.stream, std::move(source), std::move(initialize_missing));
}
void SystemImageRuntime::Publish(const std::uint32_t width, const std::uint32_t height, ImageProductBuffer::ProductSubmit submit) {
    auto& state = ActiveState();
    state.output->Publish(*state.stream, width, height, std::move(submit));
}
SystemImageWorker::SystemImageWorker(Cycle cycle, FailureSink failures, Cleanup cleanup)
    : cycle_(std::move(cycle)),
      failures_(std::move(failures)),
      cleanup_(std::move(cleanup)),
      worker_([this](const std::stop_token stop) { Run(stop); }) {
    if (!cycle_) throw std::invalid_argument("system image worker cycle is unavailable");
}
SystemImageWorker::~SystemImageWorker() { RequestStop(); }

void SystemImageWorker::Wake() noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return;
        ++wake_generation_;
    }
    ready_.notify_one();
}

void SystemImageWorker::RequestStop() noexcept {
    {
        std::scoped_lock lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    worker_.request_stop();
    ready_.notify_all();
}

void SystemImageWorker::WaitStopped() noexcept {
    std::unique_lock lock(mutex_);
    ready_.wait(lock, [this] { return stopped_; });
}

bool SystemImageWorker::stopped() const noexcept {
    std::scoped_lock lock(mutex_);
    return stopped_;
}

void SystemImageWorker::Run(const std::stop_token stop) {
    while (!stop.stop_requested()) {
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, stop, [this] { return stopping_ || wake_generation_ != 0U; });
            if (stopping_ || stop.stop_requested()) break;
            wake_generation_ = 0U;
        }
        try {
            cycle_(stop);
        } catch (...) {
            if (failures_) {
                try {
                    failures_(std::current_exception());
                } catch (...) { failures_ = {}; }
            }
        }
    }
    if (cleanup_) {
        try {
            cleanup_();
        } catch (...) {
            if (failures_) {
                try {
                    failures_(std::current_exception());
                } catch (...) {}
            }
        }
    }
    {
        std::scoped_lock lock(mutex_);
        stopped_ = true;
    }
    ready_.notify_all();
}

}  // namespace mmltk::frameworks::gpu
