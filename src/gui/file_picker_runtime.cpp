#include "file_picker.h"

#include <sys/wait.h>

#include <array>
#include <cstdio>
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

struct DialogAttempt {
    bool available = false;
    std::optional<std::string> picked;
};

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

[[nodiscard]] std::string file_uri_decode(std::string uri) {
    constexpr std::string_view kFilePrefix = "file://";
    if (uri.starts_with(kFilePrefix)) {
        uri.erase(0U, kFilePrefix.size());
    }
    std::string decoded;
    decoded.reserve(uri.size());
    for (std::size_t index = 0; index < uri.size(); ++index) {
        if (uri[index] == '%' && index + 2U < uri.size()) {
            const auto hex_value = [](const char ch) -> int {
                if (ch >= '0' && ch <= '9') {
                    return ch - '0';
                }
                if (ch >= 'a' && ch <= 'f') {
                    return 10 + ch - 'a';
                }
                if (ch >= 'A' && ch <= 'F') {
                    return 10 + ch - 'A';
                }
                return -1;
            };
            const int high = hex_value(uri[index + 1U]);
            const int low = hex_value(uri[index + 2U]);
            if (high >= 0 && low >= 0) {
                decoded.push_back(static_cast<char>((high << 4) | low));
                index += 2U;
                continue;
            }
        }
        decoded.push_back(uri[index]);
    }
    return decoded;
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

[[nodiscard]] std::string gvariant_string(const std::string_view value) {
    std::string quoted;
    quoted.reserve(value.size() + 2U);
    quoted.push_back('\'');
    for (const char ch : value) {
        if (ch == '\'' || ch == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('\'');
    return quoted;
}

[[nodiscard]] std::string gvariant_byte_array(const std::string_view value) {
    std::string bytes = "[";
    bool first = true;
    for (const unsigned char ch : value) {
        if (!first) {
            bytes += ", ";
        }
        first = false;
        bytes += "byte ";
        bytes += std::to_string(static_cast<unsigned int>(ch));
    }
    if (!first) {
        bytes += ", ";
    }
    bytes += "byte 0]";
    return bytes;
}

[[nodiscard]] std::string portal_filter_entry(const FilePickerFilter& filter) {
    std::string entry = "(" + gvariant_string(filter.name.empty() ? "Files" : filter.name) + ", [";
    bool first = true;
    for (const std::string& pattern : filter.patterns) {
        if (!first) {
            entry += ", ";
        }
        first = false;
        entry += "(0, ";
        entry += gvariant_string(pattern);
        entry += ")";
    }
    entry += "])";
    return entry;
}

[[nodiscard]] std::string portal_filters_array(std::span<const FilePickerFilter> filters) {
    std::string array = "[";
    bool first = true;
    for (const FilePickerFilter& filter : filters) {
        if (filter.patterns.empty()) {
            continue;
        }
        if (!first) {
            array += ", ";
        }
        first = false;
        array += portal_filter_entry(filter);
    }
    array += "]";
    return array;
}

void append_portal_initial_path_options(std::string& options, const std::string& initial_path,
                                        const FilePickerMode mode) {
    if (initial_path.empty()) {
        return;
    }
    std::string folder = initial_path;
    std::string name;
    if (mode != FilePickerMode::OpenFolder) {
        const std::size_t slash = initial_path.find_last_of('/');
        if (slash != std::string::npos) {
            folder = slash == 0U ? "/" : initial_path.substr(0U, slash);
            name = initial_path.substr(slash + 1U);
        } else {
            folder.clear();
            name = initial_path;
        }
    }
    if (!folder.empty()) {
        options += ", 'current_folder': <";
        options += gvariant_byte_array(folder);
        options += ">";
    }
    if (mode == FilePickerMode::SaveFile && !name.empty()) {
        options += ", 'current_name': <";
        options += gvariant_string(name);
        options += ">";
        options += ", 'current_file': <";
        options += gvariant_byte_array(initial_path);
        options += ">";
    }
}

[[nodiscard]] DialogAttempt pick_path_with_zenity(const std::string& title, const std::string& initial_path,
                                                  const FilePickerMode mode,
                                                  std::span<const FilePickerFilter> filters) {
    if (!command_exists("zenity")) {
        return {};
    }

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
        return DialogAttempt{true, std::nullopt};
    }
    if (result.exit_code != 0) {
        throw std::runtime_error("zenity file dialog failed with exit code " + std::to_string(result.exit_code));
    }
    const std::string picked = trim_copy(result.output);
    return DialogAttempt{true, picked.empty() ? std::nullopt : std::optional<std::string>{picked}};
}

[[nodiscard]] DialogAttempt pick_path_with_portal(const std::string& title, const std::string& initial_path,
                                                  const FilePickerMode mode,
                                                  std::span<const FilePickerFilter> filters) {
    if (!command_exists("gdbus")) {
        return {};
    }

    const char* method = mode == FilePickerMode::SaveFile ? "SaveFile" : "OpenFile";
    std::string options = "{'modal': <true>, 'multiple': <false>";
    if (mode == FilePickerMode::OpenFolder) {
        options += ", 'directory': <true>";
    }
    const std::string filter_options = portal_filters_array(filters);
    if (filter_options != "[]") {
        options += ", 'filters': <";
        options += filter_options;
        options += ">, 'current_filter': <";
        for (const FilePickerFilter& filter : filters) {
            if (!filter.patterns.empty()) {
                options += portal_filter_entry(filter);
                break;
            }
        }
        options += ">";
    }
    append_portal_initial_path_options(options, initial_path, mode);
    options += "}";

    std::string script =
        "handle=$(gdbus call --session --dest org.freedesktop.portal.Desktop "
        "--object-path /org/freedesktop/portal/desktop --method org.freedesktop.portal.FileChooser.";
    script += method;
    script += " '' ";
    script += shell_quote(title);
    script.push_back(' ');
    script += shell_quote(options);
    script +=
        " 2>/dev/null | sed -n \"s/^('\\''\\([^'\\'']*\\)'\\'',)$/\\1/p\"); "
        "if [ -z \"$handle\" ]; then exit 125; fi; "
        "while IFS= read -r line; do "
        "case \"$line\" in "
        "*Response*uint32\\ 0*) uri=$(printf '%s\\n' \"$line\" | grep -o \"file://[^'\\'']*\" | head -n1); "
        "if [ -n \"$uri\" ]; then printf '%s\\n' \"$uri\"; exit 0; fi; exit 125 ;; "
        "*Response*) exit 1 ;; "
        "esac; "
        "done < <(gdbus monitor --session --dest org.freedesktop.portal.Desktop --object-path \"$handle\" "
        "2>/dev/null); exit 125";

    const std::string command = "bash -lc " + shell_quote(script);

    const CommandResult result = run_command_capture(command);
    if (result.exit_code == 1) {
        return DialogAttempt{true, std::nullopt};
    }
    if (result.exit_code == 125 || result.exit_code == 127) {
        return {};
    }
    if (result.exit_code != 0) {
        throw std::runtime_error("xdg-desktop-portal file dialog failed with exit code " +
                                 std::to_string(result.exit_code));
    }
    const std::string picked_uri = trim_copy(result.output);
    return DialogAttempt{true,
                         picked_uri.empty() ? std::nullopt : std::optional<std::string>{file_uri_decode(picked_uri)}};
}

}  // namespace

std::optional<std::string> pick_path_with_dialog(const char* title, const std::string& initial_path,
                                                 const FilePickerMode mode,
                                                 const std::span<const FilePickerFilter> filters) {
    const std::string dialog_title = dialog_title_or_default(title);

    const DialogAttempt portal = pick_path_with_portal(dialog_title, initial_path, mode, filters);
    if (portal.available) {
        return portal.picked;
    }
    const DialogAttempt zenity = pick_path_with_zenity(dialog_title, initial_path, mode, filters);
    if (zenity.available) {
        return zenity.picked;
    }
    if (command_exists("gdbus") || command_exists("zenity")) {
        throw std::runtime_error(
            "no usable Linux file dialog helper is available; xdg-desktop-portal did not return a dialog and zenity "
            "is not installed or could not start");
    }
    throw std::runtime_error("no Linux file dialog helper is available; install xdg-desktop-portal/gdbus or zenity");
}

std::optional<std::string> pick_file_with_dialog(const char* title, const std::string& initial_path) {
    return pick_path_with_dialog(title, initial_path);
}

}  // namespace mmltk::gui
