#include "file_picker.h"
#include "ui_controls.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace mmltk::gui {

namespace {

std::string trim_trailing_newlines(std::string value) {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    return value;
}

} // namespace

std::optional<std::string> pick_file_with_zenity(const char* title, const std::string& initial_path) {
    std::array<int, 2> pipe_fds{-1, -1};
    if (::pipe(pipe_fds.data()) != 0) {
        throw std::runtime_error(std::string("pipe failed for zenity file picker: ") + std::strerror(errno));
    }

    const pid_t child = ::fork();
    if (child < 0) {
        const int error_number = errno;
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        throw std::runtime_error(std::string("fork failed for zenity file picker: ") + std::strerror(error_number));
    }

    if (child == 0) {
        ::close(pipe_fds[0]);
        if (::dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        ::close(pipe_fds[1]);

        std::vector<std::string> args = {
            "zenity",
            "--file-selection",
            std::string("--title=") + title,
        };
        if (!initial_path.empty()) {
            args.push_back(std::string("--filename=") + initial_path);
        }

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (std::string& arg : args) {
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        _exit(127);
    }

    ::close(pipe_fds[1]);
    std::string output;
    std::array<char, 512> buffer{};
    while (true) {
        const ssize_t bytes_read =
            ::read(pipe_fds[0], buffer.data(), buffer.size());
        if (bytes_read == 0) {
            break;
        }
        if (bytes_read < 0) {
            const int error_number = errno;
            ::close(pipe_fds[0]);
            int status = 0;
            (void)::waitpid(child, &status, 0);
            throw std::runtime_error(std::string("read failed for zenity file picker: ") + std::strerror(error_number));
        }
        output.append(buffer.data(), static_cast<size_t>(bytes_read));
    }
    ::close(pipe_fds[0]);

    int status = 0;
    if (::waitpid(child, &status, 0) < 0) {
        throw std::runtime_error(std::string("waitpid failed for zenity file picker: ") + std::strerror(errno));
    }
    if (WIFEXITED(status)) {
        const int exit_code = WEXITSTATUS(status);
        if (exit_code == 0) {
            output = trim_trailing_newlines(std::move(output));
            return output.empty() ? std::optional<std::string>{} : std::optional<std::string>{std::move(output)};
        }
        if (exit_code == 1) {
            return std::nullopt;
        }
        throw std::runtime_error("zenity file picker exited with status " + std::to_string(exit_code));
    }
    if (WIFSIGNALED(status)) {
        throw std::runtime_error("zenity file picker terminated by signal " + std::to_string(WTERMSIG(status)));
    }
    throw std::runtime_error("zenity file picker exited abnormally");
}

bool draw_file_picker_input(const char* label,
                            std::string& value,
                            bool browse_busy,
                            const char* browse_label) {
    bool browse_clicked = false;
    draw_labeled_widget(label, 0.0f, [&]() {
        const float browse_button_width = ui_scaled(96.0f);
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float input_width =
            std::max(ui_scaled(120.0f), ImGui::GetContentRegionAvail().x - browse_button_width - spacing);
        ImGui::SetNextItemWidth(input_width);
        ImGui::InputText("##value", &value);
        ImGui::SameLine();
        ImGui::BeginDisabled(browse_busy);
        browse_clicked = ImGui::Button(browse_busy ? "Opening..." : browse_label, ImVec2(browse_button_width, 0.0f));
        ImGui::EndDisabled();
    });
    return browse_clicked;
}

} // namespace mmltk::gui
