#pragma once
#include <nlohmann/json.hpp>
#include <concepts>
#include <type_traits>
#include <string>
#include <string_view>
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/view_state.h"
namespace mmltk::controller::contracts {
inline constexpr std::uint32_t kGuiSettingsSchemaVersion = 8U;
[[nodiscard]] nlohmann::json normalize_gui_settings_document(const nlohmann::json& j);
namespace settings_json_detail {
// Membership follows the canonical persisted aggregate, its workflows and their
// source selections. This projects only type membership, never external keys.
template <class T>
consteval bool adapter_type() {
    bool supported = std::same_as<T, GuiSettingsState>;
    mmltk::frameworks::reflection::visit_materialized_members<GuiSettingsState>([&]<class Declaration>(const auto&) {
        using Member = typename Declaration::member_type;
        if constexpr (std::is_class_v<Member> && !std::same_as<Member, WorkflowSettingsState>)
            supported = supported || std::same_as<T, Member>;
    });
    mmltk::frameworks::reflection::visit_materialized_members<WorkflowSettingsState>([&]<class Declaration>(const auto&) {
        using Workflow = typename Declaration::member_type;
        supported = supported || std::same_as<T, Workflow>;
        if constexpr (requires(Workflow value) { value.source; })
            supported = supported || std::same_as<T, decltype(Workflow::source)>;
    });
    return supported;
}
}  // namespace settings_json_detail
template <class T>
concept GuiSettingsJsonAdapter = settings_json_detail::adapter_type<T>();
namespace settings_json_detail {
// Each canonical type instantiates ordinary external-format conversion
// declarations. Hidden-friend lookup avoids a second handwritten type inventory.
template <GuiSettingsJsonAdapter State>
struct JsonWrite final {
    nlohmann::json& json;
    const State& state;
    friend void convert(JsonWrite);
};
template <GuiSettingsJsonAdapter State>
struct JsonRead final {
    const nlohmann::json& json;
    State& state;
    friend void convert(JsonRead);
};
}  // namespace settings_json_detail
template <GuiSettingsJsonAdapter State>
void to_json(nlohmann::json& json, const State& state) {
    convert(settings_json_detail::JsonWrite<State>{json, state});
}
template <GuiSettingsJsonAdapter State>
void from_json(const nlohmann::json& json, State& state) {
    convert(settings_json_detail::JsonRead<State>{json, state});
}
[[nodiscard]] nlohmann::json snapshot_gui_settings(const GuiSettingsState& state);
void apply_gui_settings(const nlohmann::json& j, GuiSettingsState& state);
[[nodiscard]] nlohmann::json default_gui_settings_document();
[[nodiscard]] GuiSettingsState load_initial_gui_settings_state(const std::string& path);
// Typed parse and repair classification used by the application-owned
// SettingsStore boundary.
[[nodiscard]] bool load_gui_settings_file(const std::string& path, GuiSettingsState& state, nlohmann::json* normalized_document = nullptr,
                                          bool* repair_required = nullptr);
}  // namespace mmltk::controller::contracts
