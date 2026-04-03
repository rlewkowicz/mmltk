#pragma once

#include <memory>
#include <string>

struct GLFWwindow;

namespace mmltk::gui {

class App;

struct AppDeleter {
    void operator()(App* app) const noexcept;
};

using AppHandle = std::unique_ptr<App, AppDeleter>;

void apply_app_style();
AppHandle make_app(GLFWwindow* main_window, std::string vast_api_key, std::string settings_path);
void poll_background_work(App& app);
void render(App& app);
void shutdown(App& app);

} // namespace mmltk::gui
