#pragma once
#include <cstdint>
#include <stdexcept>
#include <stop_token>
#include <system_error>
#include <utility>
#include "src/common/io/scoped_fd.h"
namespace mmltk::common::concurrency::detail {
[[nodiscard]] bool signal_cancellation_descriptor(int descriptor) noexcept;
[[nodiscard]] bool cancellation_descriptor_requested(int descriptor) noexcept;
[[nodiscard]] int duplicate_cancellation_descriptor(int descriptor, const char* purpose);
[[nodiscard]] std::pair<int, int> mint_cancellation_descriptors();
class CancellationEmitter final {
public:
 CancellationEmitter() noexcept = default;
 explicit CancellationEmitter(int descriptor) noexcept : descriptor_(descriptor) {}
 [[nodiscard]] bool valid() const noexcept { return descriptor_.get() >= 0; }
 [[nodiscard]] bool request_cancel() noexcept {
  if (!valid() || std::exchange(requested_, true)) return false;
  return signal_cancellation_descriptor(descriptor_.get());
 }
 [[nodiscard]] int duplicate_descriptor(const char* purpose) const {
  if (!valid()) throw std::logic_error("cannot duplicate an empty cancellation source");
  return duplicate_cancellation_descriptor(descriptor_.get(), purpose);
 }

private:
 mmltk::common::io::ScopedFd descriptor_;
 bool requested_ = false;
};
}  // namespace mmltk::common::concurrency::detail
namespace mmltk::common::concurrency {
template <class Tag>
class EventCancellationToken;
template <class Tag, bool CancelOnDestruction>
class EventCancellationSource;
template <class Tag>
class EventCancellationSignal final {
public:
 EventCancellationSignal() noexcept = default;
 [[nodiscard]] bool valid() const noexcept { return emitter_.valid(); }
 [[nodiscard]] bool RequestCancel() noexcept { return emitter_.request_cancel(); }

private:
 explicit EventCancellationSignal(const int descriptor) noexcept : emitter_(descriptor) {}
 detail::CancellationEmitter emitter_;
 template <class, bool>
 friend class EventCancellationSource;
};
template <class Tag>
class EventCancellationToken final {
public:
 EventCancellationToken() noexcept = default;
 [[nodiscard]] bool valid() const noexcept { return descriptor_.get() >= 0; }
 [[nodiscard]] bool cancelled() const noexcept {
  if (!valid()) return true;
  return detail::cancellation_descriptor_requested(descriptor_.get());
 }
 [[nodiscard]] int descriptor() const noexcept { return descriptor_.get(); }
 [[nodiscard]] int release() noexcept { return descriptor_.release(); }

private:
 explicit EventCancellationToken(const int descriptor) noexcept : descriptor_(descriptor) {}
 mmltk::common::io::ScopedFd descriptor_;
 template <class, bool>
 friend class EventCancellationSource;
};
template <class Tag, bool CancelOnDestruction>
class EventCancellationSource final {
public:
 using Token = EventCancellationToken<Tag>;
 using Signal = EventCancellationSignal<Tag>;
 EventCancellationSource() noexcept = default;
 ~EventCancellationSource() noexcept {
  if constexpr (CancelOnDestruction) { static_cast<void>(RequestCancel()); }
 }
 EventCancellationSource(const EventCancellationSource&) = delete;
 EventCancellationSource& operator=(const EventCancellationSource&) = delete;
 EventCancellationSource(EventCancellationSource&& other) noexcept : emitter_(std::move(other.emitter_)) {}
 EventCancellationSource& operator=(EventCancellationSource&& other) noexcept {
  if (this == &other) return *this;
  if constexpr (CancelOnDestruction) { static_cast<void>(RequestCancel()); }
  emitter_ = std::move(other.emitter_);
  return *this;
 }
 [[nodiscard]] bool valid() const noexcept { return emitter_.valid(); }
 [[nodiscard]] bool RequestCancel() noexcept { return emitter_.request_cancel(); }
 [[nodiscard]] Signal DuplicateSignal() const { return Signal{duplicate_descriptor("cancellation signal")}; }
 [[nodiscard]] EventCancellationSource DuplicateSource() const { return EventCancellationSource{duplicate_descriptor("cancellation source")}; }
 [[nodiscard]] static std::pair<EventCancellationSource, Token> Mint() {
  const auto [owner, token] = detail::mint_cancellation_descriptors();
  return {EventCancellationSource{owner}, Token{token}};
 }

private:
 explicit EventCancellationSource(const int descriptor) noexcept : emitter_(descriptor) {}
 [[nodiscard]] int duplicate_descriptor(const char* const purpose) const { return emitter_.duplicate_descriptor(purpose); }
 detail::CancellationEmitter emitter_;
};
// The callback is destroyed first, waiting for any in-flight request before the
// privately owned source and token are released.
template <class Source>
class ScopedEventCancellation final {
public:
 using Token = typename Source::Token;
 explicit ScopedEventCancellation(const std::stop_token stop) : cancellation_(Source::Mint()), callback_(stop, Request{&cancellation_.first}) {}
 ScopedEventCancellation(const ScopedEventCancellation&) = delete;
 ScopedEventCancellation& operator=(const ScopedEventCancellation&) = delete;
 ScopedEventCancellation(ScopedEventCancellation&&) = delete;
 ScopedEventCancellation& operator=(ScopedEventCancellation&&) = delete;
 [[nodiscard]] const Token& token() const noexcept { return cancellation_.second; }
 [[nodiscard]] Token ConsumeToken() noexcept { return std::move(cancellation_.second); }

private:
 struct Request final {
  Source* source;
  void operator()() const noexcept { static_cast<void>(source->RequestCancel()); }
 };
 std::pair<Source, Token> cancellation_;
 std::stop_callback<Request> callback_;
};
}  // namespace mmltk::common::concurrency
