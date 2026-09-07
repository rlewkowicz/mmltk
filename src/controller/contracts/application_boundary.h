#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>

#include "src/frameworks/reflection/reflection_metadata.h"

namespace mmltk::controller {

template <class Event>
using SystemEventSink = std::function<void(Event)>;

template <auto SnapshotMember, auto DefaultsFactory>
struct SettingsSurface final {
    static constexpr auto snapshot_member = SnapshotMember;
    static constexpr auto defaults_factory = DefaultsFactory;
};

namespace contracts::reflection {

namespace direct {

struct IntentEndpoint final {
    constexpr bool operator==(const IntentEndpoint&) const noexcept = default;
};

struct InteractionEndpoint final {
    constexpr bool operator==(const InteractionEndpoint&) const noexcept = default;
};

struct FileDialogFieldIdentity final {
    constexpr bool operator==(const FileDialogFieldIdentity&) const noexcept = default;
};

struct SettingsUpdateValues final {
    constexpr bool operator==(const SettingsUpdateValues&) const noexcept = default;
};

}  // namespace direct

struct Snapshot final {
    std::size_t byte_budget = 0U;
    constexpr bool operator==(const Snapshot&) const noexcept = default;
};

enum class EventDelivery : std::uint8_t {
    Transient,
    Critical,
    LatestState,
};

MMLTK_REFLECT_ENUM(EventDelivery)

struct Event final {
    EventDelivery delivery = EventDelivery::Transient;
    constexpr bool operator==(const Event&) const noexcept = default;
};

}  // namespace contracts::reflection

namespace contracts {

enum class ApplicationErrorCategory : std::uint8_t {
    InvalidIntent,
    Busy,
    Unavailable,
    Failed,
};

MMLTK_REFLECT_ENUM(ApplicationErrorCategory)

class ApplicationError : public std::runtime_error {
   public:
    ApplicationError(ApplicationErrorCategory category, std::string detail) : std::runtime_error(std::move(detail)), category_(category) {}

    [[nodiscard]] ApplicationErrorCategory category() const noexcept { return category_; }

   private:
    ApplicationErrorCategory category_;
};

class InvalidIntentError final : public ApplicationError {
   public:
    explicit InvalidIntentError(std::string detail) : ApplicationError(ApplicationErrorCategory::InvalidIntent, std::move(detail)) {}
};

class BusyError final : public ApplicationError {
   public:
    explicit BusyError(std::string detail) : ApplicationError(ApplicationErrorCategory::Busy, std::move(detail)) {}
};

class UnavailableError final : public ApplicationError {
   public:
    explicit UnavailableError(std::string detail) : ApplicationError(ApplicationErrorCategory::Unavailable, std::move(detail)) {}
};

class FailedError final : public ApplicationError {
   public:
    explicit FailedError(std::string detail) : ApplicationError(ApplicationErrorCategory::Failed, std::move(detail)) {}
};

}  // namespace contracts
}  // namespace mmltk::controller
