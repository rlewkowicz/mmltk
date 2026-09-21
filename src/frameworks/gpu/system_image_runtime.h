#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include "src/frameworks/gpu/image_product_pool.h"
#include "src/frameworks/gpu/image_buffer.h"
#include "src/frameworks/gpu/system_image_model.h"
#include "src/frameworks/gpu/product_revision_sequence.h"
#include "src/frameworks/gpu/device_execution.h"
namespace mmltk::frameworks::gpu {
struct SystemImageRuntimeConfig final {
 int device = -1;
 std::shared_ptr<ImageCopyBackend> backend{};
 std::unique_ptr<SystemImageModel> model{};
 DeviceContextMode context_mode = DeviceContextMode::Isolated;
 ImageProductLayout input_layout = ImageProductLayout::Clean;
 ImageProductLayout output_layout = ImageProductLayout::Clean;
 std::size_t output_buffer_count = 1U;
 ImageWorkspaceFinalize workspace_finalize{};
 int numa_node = -1;
 std::optional<DeviceExecution> execution{};
 std::shared_ptr<ImageProductRevisionSequence> product_revisions{std::make_shared<ImageProductRevisionSequence>()};
 std::optional<DeviceContext> adopted_context{};
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
  // Retained raw products and receiver leases may defer retirement. Consume completion
  // on the owner thread; the sink only wakes that owner.
  [[nodiscard]] bool deferred() const noexcept;
  [[nodiscard]] ImageStreamSettlement FinishRetirement() noexcept;
  void SetRetirementSink(std::shared_ptr<const std::function<void()>>) noexcept;

 private:
  explicit UnsafeCustody(std::shared_ptr<RetentionControl>) noexcept;
  std::shared_ptr<RetentionControl> control_;
  friend class SystemImageRuntime;
 };
 struct Retirement final {
  // False carries either terminal custody or a healthy deferred token.
  // Keep a deferred token until FinishRetirement reports its final result.
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
 [[nodiscard]] bool UsesContext(const DeviceContext&) const noexcept;
 void BindContext();
 void BeginWork();
 // A known unproved execution boundary retains exact state without issuing
 // further GPU commands. Omission performs ordinary checked settlement.
 [[nodiscard]] Retirement Retire(std::exception_ptr unproved_execution = {}) noexcept;
 [[nodiscard]] static std::optional<UnsafeCustody> UnsafeConstruction(std::exception_ptr) noexcept;
 using OutputCandidate = ImageProductPool::Candidate;
 using CompletedOutput = ImageProductPool::Product;
 [[nodiscard]] CompletedOutput Completed() const;
 [[nodiscard]] ImageProductPool::Availability ObserveOutputAvailability() const;
 [[nodiscard]] ImageProductPool::Facts OutputFacts() const;
 [[nodiscard]] ImageStorageFootprint OutputStorageFootprint() const;
 [[nodiscard]] OutputCandidate AcquireOutput(std::stop_token = {}, CompletedOutput baseline = {}, ImagePlanePreservation = ImagePlanePreservation::All);
 [[nodiscard]] OutputCandidate TryAcquireOutput(CompletedOutput& baseline, ImagePlanePreservation = ImagePlanePreservation::All);
 void Publish(OutputCandidate&, std::uint32_t, std::uint32_t, ImageProductBuffer::ProductSubmit);
 void PublishRetained(OutputCandidate&, std::uint32_t, std::uint32_t, ImageProductBuffer::ProductSubmit, ImageSubmission = ImageSubmission::Complete);
 // Wake on successful or failed GPU completion. CompleteWork consumes
 // terminal status and establishes physical settlement on the owner.
 void NotifyWorkCompletion(std::function<void()>);
 void CompleteWork();
 CompletedOutput CommitOutput(OutputCandidate&&);
 void FinalizeWorkspace(OutputCandidate&, ImageWorkspaceCoverage = {});
 void CompleteWorkspaces();
 [[nodiscard]] bool PrepareDisplay(std::uint64_t revision, const std::shared_ptr<ImageWorkspace>&);
 [[nodiscard]] bool DetachDisplay(const std::shared_ptr<ImageWorkspace>&);
 [[nodiscard]] BorrowedImageWorkspace BorrowWorkspace() const;
 [[nodiscard]] ImageWorkspaceObservation ObserveWorkspace() const;
 void SelectOutput(const CompletedOutput&);
 // Wake-only; never execute CUDA/product work from this notification.
 void SetOutputAvailableSink(std::function<void()>);
 [[nodiscard]] BorrowedImageProductReadView BorrowInput() const;
 void PublishInput(std::uint32_t, std::uint32_t, ImageProductBuffer::ProductSubmit);
 [[nodiscard]] BorrowedImageProductReadView Borrow() const;
 [[nodiscard]] SystemImageModel* model() noexcept;
 [[nodiscard]] std::array<ImageCopyPath, 2U> CopyFrom(BorrowedImageProductReadView);
 [[nodiscard]] std::array<ImageCopyPath, 2U> CopyFrom(OutputCandidate&, BorrowedImageProductReadView);
 [[nodiscard]] std::array<ImageCopyPath, 2U> CopyInputFrom(BorrowedImageProductReadView, ImageProductBuffer::MissingPlaneSubmit = {},
                                                           bool preserve_clean = false);
 void Publish(std::uint32_t width, std::uint32_t height, ImageProductBuffer::ProductSubmit);

private:
 struct State;
 [[nodiscard]] static std::shared_ptr<RetentionControl> ReserveRetention();
 [[nodiscard]] UnsafeCustody Retain(std::exception_ptr, bool deferred = false) noexcept;
 [[nodiscard]] State& ActiveState();
 [[nodiscard]] const State& ActiveState() const;
 [[nodiscard]] std::uint64_t TakeProductRevision();
 std::shared_ptr<State> state_;
 std::shared_ptr<RetentionControl> retention_;
 const std::shared_ptr<ImageProductRevisionSequence> product_revision_sequence_;
};
}  // namespace mmltk::frameworks::gpu
