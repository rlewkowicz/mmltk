#include "src/backend/data/detail/compile_admission.h"
namespace mmltk::backend::data::detail {
CompileResources CompileResources::handles(std::size_t count, bool producer, std::size_t continuation) {
 return {mmltk::common::math::checked_multiply(std::uint64_t{8192}, static_cast<std::uint64_t>(count), "benchmark handle custody overflow"), count, producer, 0, true, continuation};
}
CompileAllowance::Credits::~Credits() {
 if (charged) owner->release(*this);
}
std::uint64_t CompileAllowance::bytes() const noexcept {
 if (!credits_) return 0;
 const std::lock_guard lock(credits_->owner->mutex);
 return credits_->resources.bytes;
}
std::span<const int> CompileAllowance::cpus() const noexcept { return credits_ ? std::span<const int>(credits_->cpus) : std::span<const int>{}; }
std::size_t CompileAllowance::descriptors() const noexcept {
 if (!credits_) return 0;
 const std::lock_guard lock(credits_->owner->mutex);
 return credits_->resources.descriptors;
}
std::span<std::byte> CompileAllowance::workspace() const { return credits_ ? credits_->arena.bytes() : std::span<std::byte>{}; }
void CompileAllowance::retire_workspace() const noexcept {
 if (!credits_) return;
 auto& owner = *credits_->owner;
 std::shared_ptr<Credits> parent;
 {
  const std::lock_guard lock(owner.mutex);
  credits_->workspace_retirement_pending = true;
  parent = owner.retire_workspace(*credits_);
 }
 owner.changed.notify_all();
}
void CompileAllowance::retire_descriptors() const noexcept {
 if (!credits_) return;
 auto& owner = *credits_->owner;
 {
  const std::lock_guard lock(owner.mutex);
  const auto closed = std::exchange(credits_->resources.descriptors, 0);
  const auto unused = std::exchange(credits_->available, 0);
  const bool already_retired = std::exchange(credits_->descriptors_retired, true);
  credits_->resources.continuation_descriptors = 0;
  if (!already_retired || closed || unused) owner.return_descriptors(*credits_, closed, unused);
 }
 owner.changed.notify_all();
}
void CompileAllowance::release_unused_continuation() const noexcept {
 if (!credits_) return;
 auto& owner = *credits_->owner;
 {
  const std::lock_guard lock(owner.mutex);
  const auto unused = std::exchange(credits_->available, 0);
  const bool already_released = std::exchange(credits_->continuations_released, true);
  credits_->resources.continuation_descriptors -= unused;
  if (!already_released || unused) owner.return_descriptors(*credits_, 0, unused);
 }
 owner.changed.notify_all();
}
CompileAllowance CompileAllowance::split_storage(std::uint64_t bytes) {
 if (!credits_) return {};
 auto& owner = *credits_->owner;
 // The consumer may finish while its oversized producing stream is live.
 // This lineage is accounting only: the stream explicitly retires its own
 // capacity/CPUs when their physical backing settles, regardless of aliases.
 auto storage = std::make_shared<Credits>(credits_->owner, CompileResources{bytes, 0}, credits_);
 {
  const std::lock_guard lock(owner.mutex);
  if (credits_->resources.retained_handles || bytes > credits_->resources.bytes) throw std::invalid_argument("benchmark storage partition exceeds its admitted envelope");
  if ((credits_->arena && credits_->cpu_users) || credits_->workspace_offer || credits_->workspace_retirement_pending)
   throw std::logic_error("benchmark storage partition requires a settled input window");
  if (bytes) storage->workspace_loans.clone(credits_->workspace_loans);
  if (credits_->arena) {
   storage->resources.supplied_workspace = true;
   storage->resources.workspace_class = credits_->resources.workspace_class;
   storage->resources.workspace_window = credits_->resources.workspace_window;
   storage->arena = credits_->arena.slice(0, bytes);
   credits_->arena = credits_->arena.slice(bytes, credits_->resources.bytes - bytes);
  }
  credits_->resources.bytes -= bytes;
  if (!credits_->resources.bytes) credits_->workspace_loans.retire();
  storage->charged = true;
  owner.notify(CompileAdmission::Change::Resources);
 }
 return CompileAllowance(std::move(storage));
}
}  // namespace mmltk::backend::data::detail
