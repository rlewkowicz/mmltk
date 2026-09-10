#pragma once

#include <cstdint>

#include "src/frameworks/reflection/reflected_field_policy.h"

namespace mmltk::controller::contracts {

// Explicit acceptance control, carried only by an installed integration driver.
// These receipts never participate in ordinary product state or diagnostics.
enum class IntegrationControlKind : std::uint8_t {
    Advance,
    Settled,
    Failed,
    Progress,
    PressureEntered,
};

struct IntegrationControlReceipt final {
    IntegrationControlKind kind = IntegrationControlKind::Progress;
    [[= mmltk::frameworks::reflection::Minimum{std::uint64_t{1U}}]] std::uint64_t sequence = 0U;
    std::uint64_t progress = 0U;
    bool operator==(const IntegrationControlReceipt&) const = default;
};

MMLTK_REFLECT_ENUM(IntegrationControlKind)
MMLTK_REFLECT_FIELDS(IntegrationControlReceipt)

}  // namespace mmltk::controller::contracts
