#pragma once

#include <cstdint>
#include <limits>
#include <optional>

#include "src/controller/contracts/application_boundary.h"

namespace mmltk::controller::detail {

// Progress is replaceable. Admission retains identities for one cancellation
// request, its terminal observation, two outstanding completed frame observations,
// and a subsequent runtime-retirement failure.
// Repeated Stop has no new visible state.
class PredictRevision final {
   public:
    [[nodiscard]] static std::uint64_t Admit(const std::uint64_t current) {
        const auto next = Next(current, 5U);
        if (!next) throw contracts::FailedError("prediction observation revision exhausted");
        return *next;
    }

    [[nodiscard]] static constexpr std::optional<std::uint64_t> Progress(const std::uint64_t current,
                                                                         const bool cancellation_requested) noexcept {
        return Next(current, cancellation_requested ? 4U : 5U);
    }

    [[nodiscard]] static constexpr std::optional<std::uint64_t> Cancel(const std::uint64_t current) noexcept { return Next(current, 4U); }

    [[nodiscard]] static constexpr std::optional<std::uint64_t> Complete(const std::uint64_t current) noexcept { return Next(current, 3U); }

    [[nodiscard]] static constexpr std::optional<std::uint64_t> Frame(const std::uint64_t current, const bool active,
                                                                       const bool cancellation_requested) noexcept {
        return active ? Progress(current, cancellation_requested) : Next(current, 1U);
    }

    [[nodiscard]] static constexpr std::optional<std::uint64_t> Fail(const std::uint64_t current) noexcept { return Next(current, 0U); }

   private:
    [[nodiscard]] static constexpr std::optional<std::uint64_t> Next(const std::uint64_t current, const std::uint64_t reserved) noexcept {
        if (current >= std::numeric_limits<std::uint64_t>::max() - reserved) return std::nullopt;
        return current + 1U;
    }
};

}  // namespace mmltk::controller::detail
