#include "gui/cef_views/cef_browser_shell.h"

#include "gui/browser_runtime_cef.h"
#include "gui/cef_views/cef_browser_view_delegate.h"
#include "gui/cef_views/cef_window_delegate.h"

#include <stdexcept>

namespace mmltk::gui {

CefBrowserShell::CefBrowserShell(EmbeddedCefBrowserRuntime& runtime) : runtime_(runtime) {}

void CefBrowserShell::create_main_window() {
    CefRefPtr<CefBrowserView> browser_view =
        runtime_.create_browser_view(new CefViewsBrowserViewDelegate(runtime_, *this));
    if (browser_view == nullptr) {
        throw std::runtime_error("failed to create the main CEF browser view");
    }

    CefWindow::CreateTopLevelWindow(new CefViewsWindowDelegate(runtime_, browser_view, false, false));
}

void CefBrowserShell::create_popup_window(const CefRefPtr<CefBrowserView>& browser_view, const bool is_devtools) {
    if (browser_view == nullptr) {
        throw std::runtime_error("failed to create a popup CEF browser view");
    }

    CefWindow::CreateTopLevelWindow(new CefViewsWindowDelegate(runtime_, browser_view, true, is_devtools));
}

}  // namespace mmltk::gui
