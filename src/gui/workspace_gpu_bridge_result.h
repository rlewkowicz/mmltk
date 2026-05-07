#pragma once

#include <string>
#include <string_view>

namespace mmltk::gui {

[[nodiscard]] std::string workspace_gpu_bridge_last_export_result_code() noexcept;
[[nodiscard]] std::string workspace_gpu_bridge_last_export_result_detail() noexcept;

[[nodiscard]] std::string workspace_gpu_bridge_shared_image_export_rejected_message(std::string_view detail);

}  // namespace mmltk::gui
