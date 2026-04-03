#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

namespace mmltk::logging {

struct CliOverrides {
    std::optional<spdlog::level::level_enum> level;
    std::optional<std::filesystem::path> log_file;
    std::optional<std::filesystem::path> log_dir;
};

struct LoggingConfig {
    std::string app_name;
    std::optional<spdlog::level::level_enum> level;
    std::optional<std::filesystem::path> log_file;
    std::optional<std::filesystem::path> log_dir;
};

[[nodiscard]] LoggingConfig default_config(std::string app_name);
[[nodiscard]] LoggingConfig config_from_env(std::string app_name);
[[nodiscard]] CliOverrides scan_cli_overrides(int argc, char** argv);
[[nodiscard]] LoggingConfig merge(LoggingConfig config, const CliOverrides& overrides);
[[nodiscard]] std::optional<spdlog::level::level_enum> parse_level(std::string_view value);

void initialize(const LoggingConfig& config);

[[nodiscard]] std::shared_ptr<spdlog::logger> root_logger();
[[nodiscard]] std::shared_ptr<spdlog::logger> logger(std::string_view name);
[[nodiscard]] spdlog::level::level_enum level();

void flush();
void set_level(spdlog::level::level_enum new_level);

template <typename... Args>
inline void trace(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    root_logger()->trace(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void debug(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    root_logger()->debug(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void info(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    root_logger()->info(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void warn(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    root_logger()->warn(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void error(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    root_logger()->error(fmt, std::forward<Args>(args)...);
}

template <typename... Args>
inline void critical(spdlog::format_string_t<Args...> fmt, Args&&... args) {
    root_logger()->critical(fmt, std::forward<Args>(args)...);
}

} // namespace mmltk::logging
