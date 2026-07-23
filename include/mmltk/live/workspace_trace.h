#pragma once

#include <nlohmann/json.hpp>

#include <string_view>
#include <utility>

namespace mmltk::live {

[[nodiscard]] bool workspace_trace_enabled() noexcept;

void write_workspace_trace(std::string_view source, std::string_view event, nlohmann::json fields) noexcept;

template <typename FieldsFactory>
void trace_workspace(std::string_view source, std::string_view event, FieldsFactory&& make_fields) noexcept {
    if (!workspace_trace_enabled()) {
        return;
    }
    try {
        write_workspace_trace(source, event, std::forward<FieldsFactory>(make_fields)());
    } catch (...) {
        write_workspace_trace(source, "workspace.trace_fields_failed", nlohmann::json::object());
    }
}

}  
