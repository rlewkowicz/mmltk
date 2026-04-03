#pragma once

#include "gui/annotation/common.h"
#include "gui/annotation/annotation_ui_tokens.h"
#include "gui/ui_controls.h"

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

#include <imgui.h>

namespace mmltk::gui::annotation_ui {

class DisabledScope {
public:
    explicit DisabledScope(bool disabled,
                           std::optional<float> disabled_alpha = std::nullopt) noexcept
        : disabled_(disabled),
          pushed_alpha_(disabled_ && disabled_alpha.has_value()) {
        if (pushed_alpha_) {
            ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, *disabled_alpha);
        }
        if (disabled_) {
            ImGui::BeginDisabled();
        }
    }

    ~DisabledScope() noexcept {
        if (disabled_) {
            ImGui::EndDisabled();
        }
        if (pushed_alpha_) {
            ImGui::PopStyleVar();
        }
    }

    DisabledScope(const DisabledScope&) = delete;
    DisabledScope& operator=(const DisabledScope&) = delete;

private:
    bool disabled_ = false;
    bool pushed_alpha_ = false;
};

struct ButtonPalette {
    ImVec4 button{};
    ImVec4 hovered{};
    ImVec4 active{};
};

[[nodiscard]] inline std::optional<ImVec4> text_tone_color(TextTone tone) noexcept {
    switch (tone) {
    case TextTone::Muted:
        return ImVec4(0.58f, 0.61f, 0.66f, 1.0f);
    case TextTone::Warning:
        return ImVec4(0.93f, 0.81f, 0.49f, 1.0f);
    case TextTone::Danger:
        return ImVec4(0.94f, 0.57f, 0.50f, 1.0f);
    case TextTone::Default:
        break;
    }
    return std::nullopt;
}

class TextStyleScope {
public:
    explicit TextStyleScope(TextTone tone) noexcept {
        if (const std::optional<ImVec4> color = text_tone_color(tone);
            color.has_value()) {
            ImGui::PushStyleColor(ImGuiCol_Text, *color);
            pushed_color_ = true;
        }
    }

    ~TextStyleScope() noexcept {
        if (pushed_color_) {
            ImGui::PopStyleColor();
        }
    }

    TextStyleScope(const TextStyleScope&) = delete;
    TextStyleScope& operator=(const TextStyleScope&) = delete;

private:
    bool pushed_color_ = false;
};

[[nodiscard]] inline std::optional<ButtonPalette> button_palette(ButtonTone tone) noexcept {
    switch (tone) {
    case ButtonTone::Primary:
        return ButtonPalette{
            ImVec4(0.21f, 0.46f, 0.80f, 1.0f),
            ImVec4(0.28f, 0.54f, 0.88f, 1.0f),
            ImVec4(0.17f, 0.39f, 0.70f, 1.0f),
        };
    case ButtonTone::Danger:
        return ButtonPalette{
            ImVec4(0.67f, 0.24f, 0.20f, 1.0f),
            ImVec4(0.77f, 0.31f, 0.25f, 1.0f),
            ImVec4(0.57f, 0.19f, 0.16f, 1.0f),
        };
    case ButtonTone::Default:
        break;
    }
    return std::nullopt;
}

class ButtonStyleScope {
public:
    explicit ButtonStyleScope(ButtonTone tone) noexcept {
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
        pushed_var_count_ = 1;
        if (const std::optional<ButtonPalette> palette = button_palette(tone);
            palette.has_value()) {
            ImGui::PushStyleColor(ImGuiCol_Button, palette->button);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, palette->hovered);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, palette->active);
            pushed_color_count_ = 3;
        }
    }

    ~ButtonStyleScope() noexcept {
        if (pushed_color_count_ > 0) {
            ImGui::PopStyleColor(pushed_color_count_);
        }
        if (pushed_var_count_ > 0) {
            ImGui::PopStyleVar(pushed_var_count_);
        }
    }

    ButtonStyleScope(const ButtonStyleScope&) = delete;
    ButtonStyleScope& operator=(const ButtonStyleScope&) = delete;

private:
    int pushed_color_count_ = 0;
    int pushed_var_count_ = 0;
};

inline bool draw_button(const char* label,
                        bool enabled = true,
                        ButtonTone tone = ButtonTone::Default) {
    DisabledScope disabled_scope(!enabled, 0.55f);
    ButtonStyleScope style_scope(tone);
    return ImGui::Button(label);
}

inline bool draw_small_button(const char* label,
                              bool enabled = true,
                              ButtonTone tone = ButtonTone::Default) {
    DisabledScope disabled_scope(!enabled, 0.55f);
    ButtonStyleScope style_scope(tone);
    return ImGui::SmallButton(label);
}

inline bool draw_button(const ButtonToken& button) {
    return draw_button(button.label, button.enabled, button.tone);
}

inline bool draw_small_button(const ButtonToken& button) {
    return draw_small_button(button.label, button.enabled, button.tone);
}

inline bool draw_primary_button(const char* label, bool enabled = true) {
    return draw_button(label, enabled, ButtonTone::Primary);
}

inline bool draw_danger_button(const char* label, bool enabled = true) {
    return draw_button(label, enabled, ButtonTone::Danger);
}

template <std::size_t N>
[[nodiscard]] inline std::optional<std::size_t> draw_inline_button_row(
    const std::array<ButtonToken, N>& buttons) {
    for (std::size_t index = 0; index < N; ++index) {
        if (index > 0U) {
            ImGui::SameLine();
        }
        if (draw_button(buttons[index])) {
            return index;
        }
    }
    return std::nullopt;
}

template <typename DrawFn>
inline void draw_compact_content(ImFont* font, DrawFn&& draw_fn) {
    ::mmltk::gui::draw_with_optional_font(font, std::forward<DrawFn>(draw_fn));
}

template <typename DrawFn>
inline void draw_compact_toned_content(ImFont* font,
                                       TextTone tone,
                                       DrawFn&& draw_fn) {
    ::mmltk::gui::draw_with_optional_font(font, [&]() {
        TextStyleScope tone_scope(tone);
        std::forward<DrawFn>(draw_fn)();
    });
}

inline void draw_compact_wrapped_text(ImFont* font, const char* text) {
    ::mmltk::gui::draw_with_optional_font(font, [&]() {
        ImGui::TextWrapped("%s", text);
    });
}

inline void draw_compact_wrapped_text(ImFont* font,
                                      TextTone tone,
                                      const char* text) {
    draw_compact_toned_content(font, tone, [&]() {
        ImGui::TextWrapped("%s", text);
    });
}

template <typename... Args>
inline void draw_compact_wrapped_text(ImFont* font, const char* fmt, Args&&... args) {
    ::mmltk::gui::draw_with_optional_font(font, [&]() {
        ImGui::TextWrapped(fmt, std::forward<Args>(args)...);
    });
}

template <typename... Args>
inline void draw_compact_wrapped_text(ImFont* font,
                                      TextTone tone,
                                      const char* fmt,
                                      Args&&... args) {
    draw_compact_toned_content(font, tone, [&]() {
        ImGui::TextWrapped(fmt, std::forward<Args>(args)...);
    });
}

inline void draw_compact_text(ImFont* font, const char* text) {
    ::mmltk::gui::draw_with_optional_font(font, [&]() {
        ImGui::TextUnformatted(text);
    });
}

inline void draw_compact_disabled_hint(ImFont* font, const char* text) {
    draw_compact_wrapped_text(font, TextTone::Muted, text);
}

inline void draw_compact_box_summary(
    ImFont* font,
    const std::optional<AnnotationBox>& box,
    const char* unavailable_message = "Box: unavailable for the current shape.") {
    draw_compact_content(font, [&]() {
        if (box.has_value()) {
            ImGui::TextWrapped("Box: [%d, %d, %d, %d]",
                               box->x1,
                               box->y1,
                               box->x2,
                               box->y2);
            return;
        }
        ImGui::TextWrapped("%s", unavailable_message);
    });
}

} // namespace mmltk::gui::annotation_ui
