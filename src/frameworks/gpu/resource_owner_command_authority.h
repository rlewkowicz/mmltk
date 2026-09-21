#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
namespace mmltk::frameworks::gpu {
class ResourceOwnerWorkerCapability final {
public:
 using IsCurrent = bool (*)(const void*, std::uintptr_t) noexcept;
 using FailCurrent = bool (*)(const void*, std::uintptr_t) noexcept;
 constexpr ResourceOwnerWorkerCapability() noexcept = default;
 constexpr ResourceOwnerWorkerCapability(const void* const context, const std::uintptr_t identity, const IsCurrent is_current,
                                         const FailCurrent fail_current_callback) noexcept
     : context_(context), identity_(identity), is_current_(is_current), fail_current_(fail_current_callback) {}
 [[nodiscard]] constexpr bool valid() const noexcept { return context_ != nullptr && identity_ != 0U && is_current_ != nullptr && fail_current_ != nullptr; }
 [[nodiscard]] bool current() const noexcept { return valid() && is_current_(context_, identity_); }
 [[nodiscard]] bool fail_current() const noexcept { return valid() && fail_current_(context_, identity_); }
 [[nodiscard]] constexpr std::uintptr_t identity() const noexcept { return identity_; }

private:
 const void* context_ = nullptr;
 std::uintptr_t identity_ = 0U;
 IsCurrent is_current_ = nullptr;
 FailCurrent fail_current_ = nullptr;
};
namespace detail {
struct ResourceOwnerCommandIdentity final {
 explicit ResourceOwnerCommandIdentity(const ResourceOwnerWorkerCapability worker_in) noexcept
     : worker(worker_in), mandated_owner_thread(std::this_thread::get_id()) {}
 ResourceOwnerWorkerCapability worker{};
 std::thread::id mandated_owner_thread{};
};
inline constexpr std::size_t kMaximumResourceOwnerCommandIdentities = 3U;
inline thread_local std::array<const ResourceOwnerCommandIdentity*, kMaximumResourceOwnerCommandIdentities> current_resource_owner_commands{};
inline thread_local std::size_t current_resource_owner_command_count = 0U;
inline thread_local std::size_t resource_owner_delivery_depth = 0U;
}  // namespace detail
// The one execution-context exclusion boundary for application deliveries.
// CUDA/resource commands cannot be entered while user or product-ready code
// is running, including re-entry from a callback on an owner worker.
class ResourceOwnerDeliveryScope final {
public:
 ResourceOwnerDeliveryScope() noexcept {
  if (detail::current_resource_owner_command_count != 0U) return;
  ++detail::resource_owner_delivery_depth;
  active_ = true;
 }
 ResourceOwnerDeliveryScope(const ResourceOwnerDeliveryScope&) = delete;
 ResourceOwnerDeliveryScope& operator=(const ResourceOwnerDeliveryScope&) = delete;
 ~ResourceOwnerDeliveryScope() noexcept {
  if (active_) --detail::resource_owner_delivery_depth;
 }
 [[nodiscard]] explicit operator bool() const noexcept { return active_; }

private:
 bool active_ = false;
};
[[nodiscard]] inline bool resource_owner_delivery_active() noexcept { return detail::resource_owner_delivery_depth != 0U; }
[[nodiscard]] inline std::size_t active_resource_owner_command_count() noexcept { return detail::current_resource_owner_command_count; }
class ResourceOwnerCommandScope final {
public:
 ResourceOwnerCommandScope() noexcept = default;
 ResourceOwnerCommandScope(const ResourceOwnerCommandScope&) = delete;
 ResourceOwnerCommandScope& operator=(const ResourceOwnerCommandScope&) = delete;
 ResourceOwnerCommandScope(ResourceOwnerCommandScope&& other) noexcept
     : previous_(other.previous_),
       previous_count_(std::exchange(other.previous_count_, 0U)),
       active_(std::exchange(other.active_, false)),
       restore_(std::exchange(other.restore_, false)) {}
 ResourceOwnerCommandScope& operator=(ResourceOwnerCommandScope&&) = delete;
 ~ResourceOwnerCommandScope() noexcept {
  if (!active_ || !restore_) return;
  detail::current_resource_owner_commands = previous_;
  detail::current_resource_owner_command_count = previous_count_;
 }
 [[nodiscard]] explicit operator bool() const noexcept { return active_; }

private:
 explicit ResourceOwnerCommandScope(const detail::ResourceOwnerCommandIdentity* identity, const bool require_expected_worker) noexcept
     : previous_(detail::current_resource_owner_commands), previous_count_(detail::current_resource_owner_command_count) {
  if (identity == nullptr || resource_owner_delivery_active()) return;
  if (!identity->worker.valid()) return;
  if (require_expected_worker) {
   if (!identity->worker.current()) return;
  }
  for (std::size_t index = 0U; index != detail::current_resource_owner_command_count; ++index) {
   if (detail::current_resource_owner_commands[index] == identity) {
    active_ = true;
    return;
   }
  }
  if (detail::current_resource_owner_command_count == detail::current_resource_owner_commands.size()) return;
  detail::current_resource_owner_commands[detail::current_resource_owner_command_count++] = identity;
  active_ = true;
  restore_ = true;
 }
 std::array<const detail::ResourceOwnerCommandIdentity*, detail::kMaximumResourceOwnerCommandIdentities> previous_{};
 std::size_t previous_count_ = 0U;
 bool active_ = false;
 bool restore_ = false;
 friend class ResourceOwnerCommandAuthority;
 friend class ResourceOwnerCommandBinding;
};
// Stable application-layer identity retained by a physical aggregate and its
// bounded satellites. It contains only an opaque executor route; CUDA/device
// meaning remains entirely outside the generic executor.
class ResourceOwnerCommandBinding final {
public:
 ResourceOwnerCommandBinding() noexcept = default;
 [[nodiscard]] bool authorized() const noexcept {
  if (!valid() || resource_owner_delivery_active()) return false;
  for (std::size_t index = 0U; index != detail::current_resource_owner_command_count; ++index)
   if (detail::current_resource_owner_commands[index] == identity_.get()) return true;
  return false;
 }
 [[nodiscard]] bool valid() const noexcept { return identity_ != nullptr && identity_->worker.valid(); }
 [[nodiscard]] ResourceOwnerCommandScope EnterWorker() const noexcept {
  if (authorized()) return ResourceOwnerCommandScope{identity_.get(), false};
  return ResourceOwnerCommandScope{identity_.get(), true};
 }
 // A neutral transfer may borrow a second physical owner only after the
 // exact target owner has established the primary command scope. Naming the
 // primary binding prevents an unrelated active resource command from
 // authorizing this identity.
 [[nodiscard]] ResourceOwnerCommandScope EnterDelegated(const ResourceOwnerCommandBinding& primary) const noexcept {
  if (resource_owner_delivery_active() || !valid() || !primary.valid() || detail::current_resource_owner_command_count == 0U ||
      detail::current_resource_owner_commands[0U] != primary.identity_.get())
   return {};
  return ResourceOwnerCommandScope{identity_.get(), false};
 }
 [[nodiscard]] ResourceOwnerCommandScope EnterMandatedOwnerThread() const noexcept {
  if (resource_owner_delivery_active()) return {};
  if (authorized()) return ResourceOwnerCommandScope{identity_.get(), false};
  if (!valid() || identity_->mandated_owner_thread != std::this_thread::get_id()) return {};
  return ResourceOwnerCommandScope{identity_.get(), false};
 }
 [[nodiscard]] std::uintptr_t worker_identity() const noexcept { return valid() ? identity_->worker.identity() : 0U; }

private:
 explicit ResourceOwnerCommandBinding(std::shared_ptr<const detail::ResourceOwnerCommandIdentity> identity) noexcept : identity_(std::move(identity)) {}
 std::shared_ptr<const detail::ResourceOwnerCommandIdentity> identity_{};
 friend class ResourceOwnerCommandAuthority;
};
class ResourceOwnerCommandAuthority final {
public:
 explicit ResourceOwnerCommandAuthority(const ResourceOwnerWorkerCapability worker)
     : identity_(std::make_shared<const detail::ResourceOwnerCommandIdentity>(worker)) {}
 ResourceOwnerCommandAuthority(const ResourceOwnerCommandAuthority&) = delete;
 ResourceOwnerCommandAuthority& operator=(const ResourceOwnerCommandAuthority&) = delete;
 ResourceOwnerCommandAuthority(ResourceOwnerCommandAuthority&&) = delete;
 ResourceOwnerCommandAuthority& operator=(ResourceOwnerCommandAuthority&&) = delete;
 [[nodiscard]] ResourceOwnerCommandBinding binding() const noexcept { return ResourceOwnerCommandBinding{identity_}; }
 [[nodiscard]] bool valid() const noexcept { return identity_ != nullptr && identity_->worker.valid(); }
 [[nodiscard]] ResourceOwnerCommandScope EnterWorker() const noexcept { return binding().EnterWorker(); }
 // Called only by the physical owner at its hardware-mandated owner-thread
 // boundary. The returned scope authorizes this identity and no other.
 [[nodiscard]] ResourceOwnerCommandScope EnterMandatedOwnerThread() const noexcept {
  if (resource_owner_delivery_active()) return {};
  const auto binding = this->binding();
  if (binding.authorized()) return ResourceOwnerCommandScope{identity_.get(), false};
  if (!valid() || identity_->mandated_owner_thread != std::this_thread::get_id()) return {};
  return ResourceOwnerCommandScope{identity_.get(), false};
 }

private:
 std::shared_ptr<const detail::ResourceOwnerCommandIdentity> identity_{};
};
// Projects the primary active owner identity into the generic device-neutral
// worker failure seam. Delegated transfer owners never replace the primary
// command owner, so a physical failure can only stop the executor which
// admitted that bound operation.
[[nodiscard]] inline bool fail_current_resource_worker() noexcept {
 if (resource_owner_delivery_active() || detail::current_resource_owner_command_count == 0U) return false;
 const auto* const identity = detail::current_resource_owner_commands[0U];
 return identity != nullptr && identity->worker.fail_current();
}
}  // namespace mmltk::frameworks::gpu
