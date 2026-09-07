#pragma once

#include <string_view>

namespace mmltk::common::io {

[[nodiscard]] bool try_write_all_noexcept(int fd, std::string_view data) noexcept;
void write_all_noexcept(int fd, std::string_view data) noexcept;

}  // namespace mmltk::common::io
