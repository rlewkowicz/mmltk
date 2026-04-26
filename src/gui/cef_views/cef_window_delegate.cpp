#include "gui/cef_views/cef_window_delegate.h"

#include "gui/browser_runtime_cef.h"

#include "include/views/cef_fill_layout.h"

#include <string_view>

namespace mmltk::gui {

CefViewsWindowDelegate::CefViewsWindowDelegate(EmbeddedCefBrowserRuntime& runtime,
                                               CefRefPtr<CefBrowserView> browser_view, const bool is_popup,
                                               const bool is_devtools)
    : runtime_(runtime), browser_view_(std::move(browser_view)), is_popup_(is_popup), is_devtools_(is_devtools) {}

void CefViewsWindowDelegate::OnWindowCreated(CefRefPtr<CefWindow> window) {
    window_ = window;
    const std::string_view title = runtime_.window_title();
    window->SetTitle(CefString(title.data(), title.size()));
    window->SetToFillLayout();
    window->AddChildView(browser_view_);
    window->CenterWindow(GetPreferredSize(browser_view_));
    runtime_.on_window_created(window, is_popup_);
    window->Show();
}

void CefViewsWindowDelegate::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
    runtime_.on_window_destroyed(window, is_popup_);
    window_ = nullptr;
    browser_view_ = nullptr;
}

bool CefViewsWindowDelegate::CanClose(CefRefPtr<CefWindow>) {
    return browser_view_ == nullptr ? true : runtime_.on_window_can_close(browser_view_);
}

CefSize CefViewsWindowDelegate::GetPreferredSize(CefRefPtr<CefView>) {
    if (is_popup_) {
        return {runtime_.popup_window_width(is_devtools_), runtime_.popup_window_height(is_devtools_)};
    }
    return {runtime_.main_window_width(), runtime_.main_window_height()};
}

cef_runtime_style_t CefViewsWindowDelegate::GetWindowRuntimeStyle() {
    return CEF_RUNTIME_STYLE_ALLOY;
}

}  // namespace mmltk::gui
