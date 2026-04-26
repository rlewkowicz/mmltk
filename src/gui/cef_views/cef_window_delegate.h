#pragma once

#include "include/views/cef_browser_view.h"
#include "include/views/cef_window_delegate.h"

namespace mmltk::gui {

class EmbeddedCefBrowserRuntime;

class CefViewsWindowDelegate : public CefWindowDelegate {
   public:
    CefViewsWindowDelegate(EmbeddedCefBrowserRuntime& runtime, CefRefPtr<CefBrowserView> browser_view, bool is_popup,
                           bool is_devtools);

    void OnWindowCreated(CefRefPtr<CefWindow> window) override;
    void OnWindowDestroyed(CefRefPtr<CefWindow> window) override;
    bool CanClose(CefRefPtr<CefWindow> window) override;
    CefSize GetPreferredSize(CefRefPtr<CefView> view) override;
    cef_runtime_style_t GetWindowRuntimeStyle() override;

   private:
    EmbeddedCefBrowserRuntime& runtime_;
    CefRefPtr<CefBrowserView> browser_view_;
    CefRefPtr<CefWindow> window_;
    bool is_popup_ = false;
    bool is_devtools_ = false;

    IMPLEMENT_REFCOUNTING(CefViewsWindowDelegate);
};

}  // namespace mmltk::gui
