#pragma once

#include <cstdint>

namespace mmltk::live {

struct LiveFrameId {
    std::uint64_t session_nonce = 0;
    std::uint64_t sequence = 0;

    [[nodiscard]] bool valid() const noexcept {
        return session_nonce != 0U || sequence != 0U;
    }

    [[nodiscard]] bool operator==(const LiveFrameId& other) const noexcept {
        return session_nonce == other.session_nonce && sequence == other.sequence;
    }

    [[nodiscard]] bool operator!=(const LiveFrameId& other) const noexcept {
        return !(*this == other);
    }
};

} // namespace mmltk::live
