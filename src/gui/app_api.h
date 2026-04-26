#pragma once

#include <memory>
#include <string>

namespace mmltk::browser {

struct IntentMessage;
struct StateSnapshot;

}  // namespace mmltk::browser

namespace mmltk::gui {

class App;

struct AppDeleter {
    void operator()(App* app) const noexcept;
};

using AppHandle = std::unique_ptr<App, AppDeleter>;

AppHandle make_app(std::string vast_api_key, std::string settings_path);
void drain_background_work(App& app);
void shutdown(App& app);
[[nodiscard]] mmltk::browser::StateSnapshot browser_state_snapshot(App& app);
void apply_browser_intent(App& app, const mmltk::browser::IntentMessage& intent);

}  // namespace mmltk::gui
