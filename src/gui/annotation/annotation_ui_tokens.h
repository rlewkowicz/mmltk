#pragma once

#include <cstdint>

namespace mmltk::gui::annotation_ui {

enum class ButtonTone : std::uint8_t {
    Default,
    Primary,
    Danger,
};

enum class TextTone : std::uint8_t {
    Default,
    Muted,
    Warning,
    Danger,
};

struct ButtonToken {
    const char* label = nullptr;
    bool enabled = true;
    ButtonTone tone = ButtonTone::Default;
};

constexpr ButtonToken button_token(const char* label,
                                   bool enabled = true,
                                   ButtonTone tone = ButtonTone::Default) noexcept {
    return ButtonToken{label, enabled, tone};
}

} // namespace mmltk::gui::annotation_ui
