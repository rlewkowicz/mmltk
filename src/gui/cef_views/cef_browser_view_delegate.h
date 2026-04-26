#pragma once

#include "include/views/cef_browser_view_delegate.h"

namespace mmltk::gui {

class CefBrowserShell;
class EmbeddedCefBrowserRuntime;

class CefViewsBrowserViewDelegate : public CefBrowserViewDelegate {
   public:
    CefViewsBrowserViewDelegate(EmbeddedCefBrowserRuntime& runtime, CefBrowserShell& shell, bool is_popup = false,
                                bool is_devtools = false);

    void OnBrowserDestroyed(CefRefPtr<CefBrowserView> browser_view, CefRefPtr<CefBrowser> browser) override;
    CefRefPtr<CefBrowserViewDelegate> GetDelegateForPopupBrowserView(CefRefPtr<CefBrowserView> browser_view,
                                                                     const CefBrowserSettings& settings,
                                                                     CefRefPtr<CefClient> client,
                                                                     bool is_devtools) override;
    bool OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView> browser_view, CefRefPtr<CefBrowserView> popup_browser_view,
                                   bool is_devtools) override;
    cef_runtime_style_t GetBrowserRuntimeStyle() override;

   private:
    EmbeddedCefBrowserRuntime& runtime_;
    CefBrowserShell& shell_;
    bool is_popup_ = false;
    bool is_devtools_ = false;

    IMPLEMENT_REFCOUNTING(CefViewsBrowserViewDelegate);
};

}  // namespace mmltk::gui
