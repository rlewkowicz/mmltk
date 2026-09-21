#pragma once
#include <unistd.h>
#include <string_view>
[[nodiscard]] inline bool valid_browser_runtime_fixture_args(const int argc, char* const argv[]) noexcept {
 return argc == 10 && std::string_view{argv[1]} == "--no-remote" && std::string_view{argv[2]} == "--new-instance" && std::string_view{argv[3]} == "--profile" &&
        ::access(argv[4], R_OK | W_OK | X_OK) == 0 && std::string_view{argv[5]} == "--width" && std::string_view{argv[6]} == "1500" &&
        std::string_view{argv[7]} == "--height" && std::string_view{argv[8]} == "1125" && argv[9][0] != '\0';
}
