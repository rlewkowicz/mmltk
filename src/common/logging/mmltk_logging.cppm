module;
#include <spdlog/spdlog.h>

#include <concepts>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

export module mmltk.common.logging.mmltk_logging;

export namespace mmltk::common::logging {

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

[[nodiscard]] spdlog::level::level_enum level();
[[nodiscard]] inline bool enabled(const spdlog::level::level_enum candidate) {
    const auto active = level();
    return active != spdlog::level::off && candidate >= active;
}

void flush();
void set_level(spdlog::level::level_enum new_level);

}  // namespace mmltk::common::logging

namespace mmltk::common::logging {

[[nodiscard]] std::shared_ptr<spdlog::logger> root_logger();
[[nodiscard]] std::shared_ptr<spdlog::logger> logger(std::string_view name);

}  // namespace mmltk::common::logging

export namespace mmltk::common::logging {

template <typename Emit>
    requires std::invocable<Emit, spdlog::logger&>
inline void log_if_enabled(const spdlog::level::level_enum candidate, Emit&& emit) {
    if (!enabled(candidate)) { return; }
    auto current = root_logger();
    std::invoke(std::forward<Emit>(emit), *current);
}

template <typename Emit>
    requires std::invocable<Emit, spdlog::logger&>
inline void log_if_enabled(const std::string_view name, const spdlog::level::level_enum candidate, Emit&& emit) {
    if (!enabled(candidate)) { return; }
    auto current = logger(name);
    std::invoke(std::forward<Emit>(emit), *current);
}

template <typename Emit>
inline void trace(Emit&& emit) {
    log_if_enabled(spdlog::level::trace, std::forward<Emit>(emit));
}
template <typename Emit>
inline void debug(Emit&& emit) {
    log_if_enabled(spdlog::level::debug, std::forward<Emit>(emit));
}
template <typename Emit>
inline void info(Emit&& emit) {
    log_if_enabled(spdlog::level::info, std::forward<Emit>(emit));
}
template <typename Emit>
inline void info(const std::string_view name, Emit&& emit) {
    log_if_enabled(name, spdlog::level::info, std::forward<Emit>(emit));
}
template <typename Emit>
inline void warn(Emit&& emit) {
    log_if_enabled(spdlog::level::warn, std::forward<Emit>(emit));
}
template <typename Emit>
inline void error(Emit&& emit) {
    log_if_enabled(spdlog::level::err, std::forward<Emit>(emit));
}
template <typename Emit>
inline void error(const std::string_view name, Emit&& emit) {
    log_if_enabled(name, spdlog::level::err, std::forward<Emit>(emit));
}
template <typename Emit>
inline void critical(Emit&& emit) {
    log_if_enabled(spdlog::level::critical, std::forward<Emit>(emit));
}

}  // namespace mmltk::common::logging
