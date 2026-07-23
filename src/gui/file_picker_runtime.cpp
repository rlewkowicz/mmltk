#include "file_picker.h"

#include <sys/wait.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mmltk::gui {
namespace {

struct CommandResult {
    int exit_code = 127;
    std::string output;
};

struct DialogScope {
    std::filesystem::path container_root;
    std::filesystem::path picker_root;
    bool uses_host_mount = false;
};

constexpr std::string_view kHostMount = "/host";

[[nodiscard]] std::string trim_copy(std::string value) {
    while (!value.empty() &&
           (value.back() == '\n' || value.back() == '\r' || value.back() == ' ' || value.back() == '\t')) {
        value.pop_back();
    }
    std::size_t first = 0;
    while (first < value.size() &&
           (value[first] == '\n' || value[first] == '\r' || value[first] == ' ' || value[first] == '\t')) {
        ++first;
    }
    if (first > 0U) {
        value.erase(0U, first);
    }
    return value;
}

[[nodiscard]] std::string shell_quote(const std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2U);
    quoted.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'') {
            quoted += "'\\''";
        } else {
            quoted.push_back(ch);
        }
    }
    quoted.push_back('\'');
    return quoted;
}

[[nodiscard]] CommandResult run_command_capture(const std::string& command) {
    std::array<char, 4096U> buffer{};
    std::string output;
    FILE* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error("failed to launch file dialog helper");
    }
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const int status = ::pclose(pipe);
    int exit_code = 127;
    if (status == -1) {
        exit_code = 127;
    } else if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        exit_code = 128 + WTERMSIG(status);
    }
    return CommandResult{exit_code, std::move(output)};
}

[[nodiscard]] bool command_exists(const char* command_name) {
    const CommandResult result =
        run_command_capture(std::string{"command -v "} + shell_quote(command_name) + " >/dev/null 2>&1");
    return result.exit_code == 0;
}

[[nodiscard]] std::string dialog_title_or_default(const char* title) {
    if (title != nullptr && std::string_view{title}.size() > 0U) {
        return std::string{title};
    }
    return "Select path";
}

[[nodiscard]] std::string zenity_filter_args(std::span<const FilePickerFilter> filters) {
    std::string args;
    for (const FilePickerFilter& filter : filters) {
        if (filter.patterns.empty()) {
            continue;
        }
        std::string spec = filter.name.empty() ? "Files" : filter.name;
        spec += " |";
        for (const std::string& pattern : filter.patterns) {
            spec.push_back(' ');
            spec += pattern;
        }
        args += " --file-filter=";
        args += shell_quote(spec);
    }
    return args;
}

[[nodiscard]] std::string directory_argument(const std::filesystem::path& directory) {
    std::string path = directory.string();
    if (!path.ends_with('/')) {
        path.push_back('/');
    }
    return path;
}

[[nodiscard]] bool has_parent_escape(const std::filesystem::path& relative) {
    const auto first = relative.begin();
    return first != relative.end() && *first == "..";
}

[[nodiscard]] bool path_is_within(const std::filesystem::path& root, const std::filesystem::path& candidate) {
    const std::filesystem::path relative = candidate.lexically_relative(root);
    return !relative.empty() && !relative.is_absolute() && !has_parent_escape(relative);
}

[[nodiscard]] std::filesystem::path canonicalized(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path result = std::filesystem::weakly_canonical(path, error);
    if (error) {
        result = std::filesystem::absolute(path, error);
    }
    if (error) {
        throw std::runtime_error("failed to resolve selected path: " + error.message());
    }
    return result.lexically_normal();
}

[[nodiscard]] DialogScope dialog_scope() {
    DialogScope scope;
    scope.container_root = canonicalized(std::filesystem::current_path());
    const std::filesystem::path host_mount{kHostMount};
    const std::filesystem::path relative = scope.container_root.lexically_relative(host_mount);
    scope.uses_host_mount = !relative.empty() && !relative.is_absolute() && !has_parent_escape(relative);
    scope.picker_root = scope.uses_host_mount ? std::filesystem::path{"/"} / relative : scope.container_root;
    return scope;
}

[[nodiscard]] std::string resolve_picked_path(const std::string& picked, const DialogScope& scope) {
    std::filesystem::path host_path{picked};
    if (host_path.is_relative()) {
        host_path = scope.picker_root / host_path;
    }

    std::filesystem::path candidate = canonicalized(host_path);
    if (path_is_within(scope.container_root, candidate)) {
        return candidate.string();
    }
    if (scope.uses_host_mount && host_path.is_absolute()) {
        candidate = canonicalized(std::filesystem::path{kHostMount} / host_path.relative_path());
        if (path_is_within(scope.container_root, candidate)) {
            return candidate.string();
        }
    }
    throw std::runtime_error("selected path must remain inside the launch directory: " + scope.container_root.string());
}

[[nodiscard]] std::optional<std::string> pick_path_with_zenity(const std::string& title,
                                                               const std::string& initial_path,
                                                               const FilePickerMode mode,
                                                               std::span<const FilePickerFilter> filters) {
    std::string command = "zenity --file-selection --title=" + shell_quote(title);
    if (!initial_path.empty()) {
        command += " --filename=" + shell_quote(initial_path);
    }
    switch (mode) {
        case FilePickerMode::OpenFile:
            break;
        case FilePickerMode::OpenFolder:
            command += " --directory";
            break;
        case FilePickerMode::SaveFile:
            command += " --save --confirm-overwrite";
            break;
    }
    command += zenity_filter_args(filters);
    command += " 2>/dev/null";

    const CommandResult result = run_command_capture(command);
    if (result.exit_code == 1) {
        return std::nullopt;
    }
    if (result.exit_code != 0) {
        throw std::runtime_error("zenity file dialog failed with exit code " + std::to_string(result.exit_code));
    }
    const std::string picked = trim_copy(result.output);
    return picked.empty() ? std::nullopt : std::optional<std::string>{picked};
}

}  

std::optional<std::string> pick_path_with_dialog(const char* title, const FilePickerMode mode,
                                                 const std::span<const FilePickerFilter> filters) {
    if (!command_exists("zenity")) {
        throw std::runtime_error("the container is missing the required zenity file dialog helper");
    }
    const DialogScope scope = dialog_scope();
    const std::optional<std::string> picked =
        pick_path_with_zenity(dialog_title_or_default(title), directory_argument(scope.picker_root), mode, filters);
    if (!picked.has_value()) {
        return std::nullopt;
    }
    return resolve_picked_path(*picked, scope);
}

std::optional<std::string> pick_file_with_dialog(const char* title) {
    return pick_path_with_dialog(title);
}

}  
