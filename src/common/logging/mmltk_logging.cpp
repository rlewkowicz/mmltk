module;
#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "src/common/system/runtime_paths.h"
#include "src/common/types/string_utils.h"
module mmltk.common.logging.mmltk_logging;
namespace mmltk::common::logging {
namespace {
constexpr const char* kDefaultPattern = "%Y-%m-%d %H:%M:%S.%e [%P:%t] [%n] [%^%l%$] %v";
constexpr std::size_t kLogRotationBytes = std::size_t{10} * std::size_t{1024} * std::size_t{1024};
constexpr std::size_t kLogRotationFiles = 5U;
std::mutex g_mutex;
std::shared_ptr<spdlog::logger> g_root_logger;
std::vector<spdlog::sink_ptr> g_sinks;
spdlog::sink_ptr g_fatal_file_sink;
std::string g_app_name;
std::atomic<spdlog::level::level_enum> g_level{spdlog::level::off};
bool is_known_level_name(std::string_view value) {
    const std::string lowered = mmltk::common::types::to_lower(value);
    return lowered == "trace" || lowered == "debug" || lowered == "info" || lowered == "warn" || lowered == "warning" || lowered == "error" ||
           lowered == "err" || lowered == "critical" || lowered == "off";
}
std::optional<spdlog::level::level_enum> parse_level_impl(std::string_view value) {
    if (value.empty()) { return std::nullopt; }
    const std::string normalized(value);
    const spdlog::level::level_enum parsed = spdlog::level::from_str(normalized);
    if (!is_known_level_name(normalized)) { throw std::runtime_error("invalid MMLTK log level: " + normalized); }
    return parsed;
}
std::optional<std::filesystem::path> parse_env_path(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') { return std::nullopt; }
    return std::filesystem::path(value);
}
std::filesystem::path sanitize_log_filename(std::string_view app_name) {
    std::string filename(app_name);
    for (char& ch : filename) {
        if (ch == '/' || ch == '\\' || ch == ':' || ch == ' ') { ch = '_'; }
    }
    if (filename.empty()) { filename = "mmltk"; }
    return {filename + ".log"};
}
std::filesystem::path resolve_relative_to_install_prefix(const std::filesystem::path& path) {
    if (path.is_absolute()) { return path; }
    return mmltk::common::system::runtime_paths::install_prefix() / path;
}
std::filesystem::path resolve_log_file_path(const LoggingConfig& config) {
    const std::filesystem::path default_dir = mmltk::common::system::runtime_paths::install_prefix() / ".mmltk-data" / "logs";
    if (config.log_file.has_value()) {
        std::filesystem::path path = *config.log_file;
        if (!path.is_absolute()) { path = resolve_relative_to_install_prefix(path); }
        return path;
    }
    std::filesystem::path dir = default_dir;
    if (config.log_dir.has_value()) {
        dir = *config.log_dir;
        if (!dir.is_absolute()) { dir = resolve_relative_to_install_prefix(dir); }
    }
    return dir / sanitize_log_filename(config.app_name);
}
void ensure_parent_directory(const std::filesystem::path& path) {
    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) { std::filesystem::create_directories(parent); }
}
spdlog::level::level_enum default_runtime_level() {
#if defined(MMLTK_BUILD_CONFIG)
    if (std::string_view(MMLTK_BUILD_CONFIG) == "Dev") { return spdlog::level::debug; }
#endif
    return spdlog::level::info;
}
void install_logger_locked(const LoggingConfig& config) {
    const spdlog::level::level_enum runtime_level = config.enabled() ? config.level.value_or(default_runtime_level()) : spdlog::level::off;
    g_level.store(spdlog::level::off, std::memory_order_relaxed);
    if (g_root_logger != nullptr) {
        spdlog::shutdown();
        spdlog::drop_all();
    }
    g_root_logger.reset();
    g_sinks.clear();
    g_fatal_file_sink.reset();
    if (runtime_level == spdlog::level::off) { return; }
    spdlog::set_pattern(kDefaultPattern);
    spdlog::set_level(runtime_level);
    spdlog::flush_on(spdlog::level::warn);
    const std::filesystem::path log_path = resolve_log_file_path(config);
    ensure_parent_directory(log_path);
    g_sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    g_sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(log_path.string(), kLogRotationBytes, kLogRotationFiles, true));
    auto root = std::make_shared<spdlog::logger>(config.app_name, g_sinks.begin(), g_sinks.end());
    spdlog::initialize_logger(root);
    root->set_level(runtime_level);
    root->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(root);
    g_app_name = config.app_name;
    g_level = runtime_level;
    g_root_logger = std::move(root);
    g_fatal_file_sink = g_sinks.back();
}
}  // namespace
LoggingConfig default_config(std::string app_name) {
    LoggingConfig config;
    config.app_name = std::move(app_name);
    return config;
}
LoggingConfig config_from_env(std::string app_name) {
    LoggingConfig config = default_config(std::move(app_name));
    const char* level_value = std::getenv("MMLTK_LOG_LEVEL");
    if (level_value != nullptr && level_value[0] != '\0') { config.level = parse_level_impl(level_value); }
    config.log_file = parse_env_path("MMLTK_LOG_FILE");
    config.log_dir = parse_env_path("MMLTK_LOG_DIR");
    return config;
}
CliOverrides scan_cli_overrides(int argc, char** argv) {
    CliOverrides overrides;
    for (int index = 1; index < argc; ++index) {
        const std::string_view arg = argv[index];
        auto capture_next = [&](std::optional<std::filesystem::path>& out) {
            if (index + 1 < argc) { out = std::filesystem::path(argv[++index]); }
        };
        auto capture_next_level = [&]() {
            if (index + 1 < argc) { overrides.level = parse_level(argv[++index]); }
        };
        if (arg.starts_with("--log-level=")) {
            overrides.level = parse_level_impl(arg.substr(std::string_view("--log-level=").size()));
            continue;
        }
        if (arg == "--log-level") {
            capture_next_level();
            continue;
        }
        if (arg.starts_with("--log-file=")) {
            overrides.log_file = std::filesystem::path(arg.substr(std::string_view("--log-file=").size()));
            continue;
        }
        if (arg == "--log-file") {
            capture_next(overrides.log_file);
            continue;
        }
        if (arg.starts_with("--log-dir=")) {
            overrides.log_dir = std::filesystem::path(arg.substr(std::string_view("--log-dir=").size()));
            continue;
        }
        if (arg == "--log-dir") {
            capture_next(overrides.log_dir);
            continue;
        }
    }
    return overrides;
}
std::optional<spdlog::level::level_enum> parse_level(std::string_view value) { return parse_level_impl(value); }
LoggingConfig merge(LoggingConfig config, const CliOverrides& overrides) {
    if (overrides.level.has_value()) { config.level = overrides.level; }
    if (overrides.log_file.has_value()) { config.log_file = overrides.log_file; }
    if (overrides.log_dir.has_value()) { config.log_dir = overrides.log_dir; }
    return config;
}
void initialize(const LoggingConfig& config) {
    if (config.enabled() && config.app_name.empty()) { throw std::runtime_error("logging initialization requires a non-empty app name"); }
    std::lock_guard<std::mutex> lock(g_mutex);
    install_logger_locked(config);
}
std::shared_ptr<spdlog::logger> root_logger() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_root_logger;
}
std::shared_ptr<spdlog::logger> logger(std::string_view name) {
    if (name.empty()) { return root_logger(); }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_root_logger == nullptr) { return nullptr; }
    if (name == g_app_name) { return g_root_logger; }
    if (auto existing = spdlog::get(std::string(name)); existing != nullptr) { return existing; }
    auto named = std::make_shared<spdlog::logger>(std::string(name), g_sinks.begin(), g_sinks.end());
    spdlog::initialize_logger(named);
    named->set_level(g_level.load(std::memory_order_relaxed));
    named->flush_on(spdlog::level::warn);
    return named;
}
spdlog::level::level_enum level() { return g_level.load(std::memory_order_relaxed); }
void report_fatal(const std::string_view component, const std::string_view detail, const std::optional<int> status,
                  const std::string_view diagnostic_logger) noexcept {
    const int saved_errno = errno;
    // The maximum record is 997 bytes, including truncation markers, an int
    // status and newline; the fixed budget also stays below Linux PIPE_BUF.
    std::array<char, 1024> buffer{};
    std::size_t size = 0U;
    const auto append = [&](const std::string_view text, const std::size_t limit) {
        const std::size_t count = std::min(text.size(), limit);
        for (std::size_t index = 0U; index < count; ++index) {
            const unsigned char character = static_cast<unsigned char>(text[index]);
            buffer[size++] = character < 32U || character == 127U ? ' ' : static_cast<char>(character);
        }
        if (text.size() > count) {
            buffer[size++] = '.';
            buffer[size++] = '.';
            buffer[size++] = '.';
        }
    };
    append("fatal: ", 7U);
    append(component, 160U);
    append(": ", 2U);
    append(detail, 800U);
    if (status) {
        append(" (status=", 9U);
        const auto result = std::to_chars(buffer.data() + size, buffer.data() + buffer.size() - 2U, *status);
        size = static_cast<std::size_t>(result.ptr - buffer.data());
        buffer[size++] = ')';
    }
    buffer[size++] = '\n';
    sigset_t blocked{}, previous{}, pending{};
    if (::sigemptyset(&blocked) == 0 && ::sigaddset(&blocked, SIGPIPE) == 0 && ::pthread_sigmask(SIG_BLOCK, &blocked, &previous) == 0) {
        // A pending signal belongs to the caller. Only consume a new SIGPIPE
        // caused by this write, while it is blocked on this thread alone.
        if (::sigpending(&pending) == 0) {
            const bool already_pending = ::sigismember(&pending, SIGPIPE) == 1;
            std::size_t written = 0U;
            unsigned int interruptions = 0U;
            bool broken_pipe = false;
            while (written < size) {
                const ssize_t count = ::write(STDERR_FILENO, buffer.data() + written, size - written);
                if (count > 0)
                    written += static_cast<std::size_t>(count);
                else if (count < 0 && errno == EINTR && ++interruptions < 4U)
                    continue;
                else {
                    broken_pipe = count < 0 && errno == EPIPE;
                    break;
                }
            }
            if (broken_pipe && !already_pending) {
                const timespec immediate{};
                int consumed = -1;
                do { consumed = ::sigtimedwait(&blocked, nullptr, &immediate); } while (consumed < 0 && errno == EINTR);
            }
        }
        static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous, nullptr));
    }
    // Terminal visibility does not depend on logger initialization or sink health.
    // Address only the file sink so configured stderr logging cannot duplicate it.
    if (enabled(spdlog::level::critical)) {
        try {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_fatal_file_sink && g_level.load(std::memory_order_relaxed) != spdlog::level::off) {
                const auto name = (diagnostic_logger.empty() ? std::string_view{g_app_name} : diagnostic_logger).substr(0U, 160U);
                const spdlog::details::log_msg message{name, spdlog::level::critical, {buffer.data(), size - 1U}};
                g_fatal_file_sink->log(message);
                g_fatal_file_sink->flush();
            }
        } catch (...) {}
    }
    errno = saved_errno;
}
void flush() {
    if (level() == spdlog::level::off) { return; }
    if (auto current = root_logger(); current != nullptr) { current->flush(); }
    spdlog::apply_all([](const std::shared_ptr<spdlog::logger>& current) {
        if (current != nullptr) { current->flush(); }
    });
}
void set_level(spdlog::level::level_enum new_level) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_root_logger == nullptr) {
        auto config = config_from_env(g_app_name.empty() ? "mmltk" : g_app_name);
        config.level = new_level;
        install_logger_locked(config);
        return;
    }
    g_level = new_level;
    spdlog::set_level(new_level);
    spdlog::apply_all([new_level](const std::shared_ptr<spdlog::logger>& current) {
        if (current != nullptr) { current->set_level(new_level); }
    });
}
}  // namespace mmltk::common::logging
