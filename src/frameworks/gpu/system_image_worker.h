#pragma once

#include <atomic>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "src/frameworks/gpu/image_buffer.h"

namespace mmltk::frameworks::gpu {

class SystemImageModel {
   public:
    struct Release final {
        bool all_released = true;
        std::exception_ptr failure{};
    };
    virtual ~SystemImageModel() = default;
    virtual void StopIngress() noexcept {}
    [[nodiscard]] virtual Release ReleaseResources() noexcept { return {}; }
};

struct SystemImageRuntimeConfig final {
    int device = -1;
    std::shared_ptr<ImageCopyBackend> backend{};
    std::unique_ptr<SystemImageModel> model{};
    DeviceContextMode context_mode = DeviceContextMode::Isolated;
    ImageProductLayout input_layout = ImageProductLayout::Clean;
    ImageProductLayout output_layout = ImageProductLayout::Clean;
    std::size_t output_buffer_count = 1U;
    int numa_node = -1;
    std::optional<DeviceExecution> execution{};
};

class SystemImageRuntime final {
   private:
    struct RetentionControl;

   public:
    class UnsafeCustody final {
       public:
        UnsafeCustody() noexcept = default;
        UnsafeCustody(const UnsafeCustody&) = delete;
        UnsafeCustody& operator=(const UnsafeCustody&) = delete;
        UnsafeCustody(UnsafeCustody&&) noexcept = default;
        UnsafeCustody& operator=(UnsafeCustody&&) noexcept = default;
        [[nodiscard]] bool valid() const noexcept;
        [[nodiscard]] std::exception_ptr failure() const noexcept;

       private:
        explicit UnsafeCustody(std::shared_ptr<RetentionControl>) noexcept;
        std::shared_ptr<RetentionControl> control_;
        friend class SystemImageRuntime;
    };
    struct Retirement final {
        bool safe_to_destroy = false;
        std::exception_ptr failure{};
        UnsafeCustody custody{};
    };

    explicit SystemImageRuntime(SystemImageRuntimeConfig);
    ~SystemImageRuntime() noexcept;
    SystemImageRuntime(const SystemImageRuntime&) = delete;
    SystemImageRuntime& operator=(const SystemImageRuntime&) = delete;
    SystemImageRuntime(SystemImageRuntime&&) = delete;
    SystemImageRuntime& operator=(SystemImageRuntime&&) = delete;
    [[nodiscard]] int device() const noexcept;
    [[nodiscard]] const DeviceExecution* execution() const noexcept;
    void BindContext();
    void BeginWork();
    [[nodiscard]] Retirement Retire() noexcept;
    [[nodiscard]] static std::optional<UnsafeCustody> UnsafeConstruction(std::exception_ptr) noexcept;
    [[nodiscard]] ImageProductBuffer& output();
    [[nodiscard]] const ImageProductBuffer& output() const;
    using OutputCandidate = ImageProductPool::Candidate;
    using CompletedOutput = ImageProductPool::Product;
    [[nodiscard]] OutputCandidate AcquireOutput(std::stop_token = {});
    void Publish(OutputCandidate&, std::uint32_t, std::uint32_t, ImageCandidateInitialization, ImageProductBuffer::ProductSubmit);
    CompletedOutput CommitOutput(OutputCandidate&&);
    void SelectOutput(const CompletedOutput&);
    void SetOutputAvailableSink(std::function<void()>);
    void SetProductRevisionSequence(std::shared_ptr<std::atomic<std::uint64_t>>);
    [[nodiscard]] BorrowedImageProductReadView BorrowInput() const;
    [[nodiscard]] BorrowedImageProductReadView Borrow() const;
    [[nodiscard]] SystemImageModel* model() noexcept;
    [[nodiscard]] std::array<ImageCopyPath, 2U> CopyFrom(BorrowedImageProductReadView);
    [[nodiscard]] std::array<ImageCopyPath, 2U> CopyInputFrom(BorrowedImageProductReadView, ImageProductBuffer::MissingPlaneSubmit = {});
    void Publish(std::uint32_t width, std::uint32_t height, ImageProductBuffer::ProductSubmit);

   private:
    struct State;
    [[nodiscard]] static std::shared_ptr<RetentionControl> ReserveRetention();
    [[nodiscard]] UnsafeCustody Retain(std::exception_ptr) noexcept;
    [[nodiscard]] State& ActiveState();
    [[nodiscard]] const State& ActiveState() const;
    [[nodiscard]] std::uint64_t TakeProductRevision();
    std::shared_ptr<State> state_;
    std::shared_ptr<RetentionControl> retention_;
    std::shared_ptr<std::atomic<std::uint64_t>> product_revision_sequence_;
};

class SystemImageWorker final {
   public:
    using Cycle = std::function<void(std::stop_token)>;
    using FailureSink = std::function<void(std::exception_ptr)>;
    using Cleanup = std::function<void()>;

    SystemImageWorker(Cycle, FailureSink, Cleanup = {});
    ~SystemImageWorker();
    SystemImageWorker(const SystemImageWorker&) = delete;
    SystemImageWorker& operator=(const SystemImageWorker&) = delete;

    void Wake() noexcept;
    void RequestStop() noexcept;
    void WaitStopped() noexcept;
    [[nodiscard]] bool stopped() const noexcept;

   private:
    void Run(std::stop_token);
    Cycle cycle_;
    FailureSink failures_;
    Cleanup cleanup_;
    mutable std::mutex mutex_;
    std::condition_variable_any ready_;
    bool stopping_ = false;
    bool stopped_ = false;
    std::uint64_t wake_generation_ = 0U;
    std::jthread worker_;
};

}  // namespace mmltk::frameworks::gpu
