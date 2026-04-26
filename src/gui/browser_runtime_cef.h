#pragma once

#include "gui/app_options.h"

#include "include/cef_browser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_browser_view_delegate.h"
#include "include/views/cef_window.h"

#include <memory>
#include <string_view>

namespace mmltk::gui {

class App;
class CefBrowserShell;

class EmbeddedCefBrowserRuntime {
   public:
    EmbeddedCefBrowserRuntime(App& app, const AppLaunchOptions& options);
    ~EmbeddedCefBrowserRuntime();

    EmbeddedCefBrowserRuntime(const EmbeddedCefBrowserRuntime&) = delete;
    EmbeddedCefBrowserRuntime& operator=(const EmbeddedCefBrowserRuntime&) = delete;

    void attach_shell(CefBrowserShell& shell);
    [[nodiscard]] bool initialize(int argc, char** argv);
    void run_message_loop();
    void shutdown();

    [[nodiscard]] int exit_code() const noexcept;
    [[nodiscard]] int main_window_width() const noexcept;
    [[nodiscard]] int main_window_height() const noexcept;
    [[nodiscard]] int popup_window_width(bool is_devtools) const noexcept;
    [[nodiscard]] int popup_window_height(bool is_devtools) const noexcept;
    [[nodiscard]] std::string_view window_title() const noexcept;

    [[nodiscard]] CefRefPtr<CefBrowserView> create_browser_view(const CefRefPtr<CefBrowserViewDelegate>& delegate);
    void on_window_created(const CefRefPtr<CefWindow>& window, bool is_popup);
    void on_window_destroyed(const CefRefPtr<CefWindow>& window, bool is_popup);
    [[nodiscard]] bool on_window_can_close(const CefRefPtr<CefBrowserView>& browser_view) const;
    [[nodiscard]] CefRefPtr<CefBrowser> browser();

    class Impl;

   private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::gui
