#include "app_api.h"
#include "cli_seed.h"
#include "app_options.h"
#include "execution_policy.h"
#include "mmltk_logging.h"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include <GLFW/glfw3.h>

#include <cstdio>
#include <exception>
#include <cstdlib>
#include <cstring>

namespace {

void glfw_error_callback(int error, const char* description) {
    mmltk::logging::logger("gui")->error("glfw error {}: {}", error, description);
}

const char* env_or_unset(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : "<unset>";
}

bool env_is_set(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
}

void report_gui_error(std::string_view message) noexcept {
    try {
        mmltk::logging::logger("gui")->error("{}", message);
        return;
    } catch (...) {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
        return;
    }
}

#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
const char* glfw_platform_name(int platform) {
    switch (platform) {
    case GLFW_PLATFORM_WAYLAND:
        return "wayland";
    case GLFW_PLATFORM_X11:
        return "x11";
    case GLFW_ANY_PLATFORM:
        return "auto";
    default:
        return "unknown";
    }
}
#endif

void print_display_environment_hint() {
    mmltk::logging::logger("gui")->warn(
        "display env: XDG_SESSION_TYPE={} XDG_RUNTIME_DIR={} WAYLAND_DISPLAY={} DISPLAY={} XAUTHORITY={} __NV_PRIME_RENDER_OFFLOAD={} __GLX_VENDOR_LIBRARY_NAME={} MMLTK_GUI_PLATFORM={}",
        env_or_unset("XDG_SESSION_TYPE"),
        env_or_unset("XDG_RUNTIME_DIR"),
        env_or_unset("WAYLAND_DISPLAY"),
        env_or_unset("DISPLAY"),
        env_or_unset("XAUTHORITY"),
        env_or_unset("__NV_PRIME_RENDER_OFFLOAD"),
        env_or_unset("__GLX_VENDOR_LIBRARY_NAME"),
        env_or_unset("MMLTK_GUI_PLATFORM"));
    mmltk::logging::logger("gui")->warn(
        "For Linux/NVIDIA CUDA-OpenGL interop, prefer Xwayland/NVIDIA env values when DISPLAY is available: "
        "XAUTHORITY=/run/user/1000/.mutter-Xwaylandauth.* __NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia "
        "MMLTK_GUI_PLATFORM=x11.");
}

void configure_platform_hint_from_session() {
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
    const bool x11_supported = glfwPlatformSupported(GLFW_PLATFORM_X11) == GLFW_TRUE;
    const bool wayland_supported = glfwPlatformSupported(GLFW_PLATFORM_WAYLAND) == GLFW_TRUE;
    const char* requested_platform = std::getenv("MMLTK_GUI_PLATFORM");

    if (requested_platform != nullptr && requested_platform[0] != '\0' &&
        std::strcmp(requested_platform, "auto") != 0) {
        if (std::strcmp(requested_platform, "x11") == 0 && x11_supported) {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        } else if (std::strcmp(requested_platform, "wayland") == 0 && wayland_supported) {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
        } else {
            mmltk::logging::logger("gui")->warn("MMLTK_GUI_PLATFORM={} is not supported by this GLFW build",
                                                requested_platform);
        }
        return;
    }

    // Prefer X11/GLX when an X display is present. This keeps native Wayland support
    // available, but it avoids hard-forcing a Wayland EGL path on NVIDIA interop setups.
    if (env_is_set("DISPLAY") && x11_supported) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
        return;
    }
    if (env_is_set("WAYLAND_DISPLAY") && wayland_supported) {
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_WAYLAND);
    }
#endif
}

} // namespace

int main(int argc, char** argv) {
    try {
        mmltk::logging::initialize(mmltk::logging::merge(mmltk::logging::config_from_env("mmltk_gui"),
                                                         mmltk::logging::scan_cli_overrides(argc, argv)));
        const mmltk::ExecutionPolicySnapshot execution_snapshot =
            mmltk::apply_process_execution_policy();
        mmltk::log_process_execution_policy("mmltk_gui", execution_snapshot, false, true);
    } catch (const std::exception& error) {
        report_gui_error(std::string("mmltk gui error: ") + error.what());
        return 1;
    }

    mmltk::gui::AppLaunchOptions launch_options;
    try {
        launch_options = mmltk::gui::parse_app_launch_options(argc, argv);
        if (launch_options.seed_from_cli) {
            mmltk::gui::apply_gui_cli_seed_file(launch_options.settings_path, launch_options.seed_cli_args);
        }
    } catch (const std::exception& error) {
        report_gui_error(std::string("mmltk gui error: ") + error.what());
        return 1;
    }

    glfwSetErrorCallback(glfw_error_callback);
    configure_platform_hint_from_session();
    if (glfwInit() == 0) {
        mmltk::logging::logger("gui")->error("failed to initialize GLFW");
        print_display_environment_hint();
        return 1;
    }

#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
    mmltk::logging::logger("gui")->info("glfw platform: {}", glfw_platform_name(glfwGetPlatform()));
#endif

    constexpr const char* glsl_version = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(1600, 960, "mmltk GUI", nullptr, nullptr);
    if (window == nullptr) {
        glfwTerminate();
        mmltk::logging::logger("gui")->error("failed to create GLFW window");
        print_display_environment_hint();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "imgui.ini";

    mmltk::gui::apply_app_style();

    if (!ImGui_ImplGlfw_InitForOpenGL(window, true)) {
        glfwDestroyWindow(window);
        glfwTerminate();
        mmltk::logging::logger("gui")->error("failed to initialize ImGui GLFW backend");
        return 1;
    }
    if (!ImGui_ImplOpenGL3_Init(glsl_version)) {
        ImGui_ImplGlfw_Shutdown();
        glfwDestroyWindow(window);
        glfwTerminate();
        mmltk::logging::logger("gui")->error("failed to initialize ImGui OpenGL backend");
        return 1;
    }

    mmltk::gui::AppHandle app =
        mmltk::gui::make_app(window, launch_options.vast_api_key, launch_options.settings_path);
    while (glfwWindowShouldClose(window) == 0) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        mmltk::gui::poll_background_work(*app);
        mmltk::gui::render(*app);

        ImGui::Render();

        int framebuffer_width = 0;
        int framebuffer_height = 0;
        glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
        glViewport(0, 0, framebuffer_width, framebuffer_height);
        glClearColor(0.06f, 0.07f, 0.08f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    mmltk::gui::shutdown(*app);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
