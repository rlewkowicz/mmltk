#include "gui/cef_views/cef_browser_view_delegate.h"

#include "gui/cef_views/cef_browser_shell.h"

#include "include/views/cef_window.h"

namespace mmltk::gui {

CefViewsBrowserViewDelegate::CefViewsBrowserViewDelegate(EmbeddedCefBrowserRuntime& runtime, CefBrowserShell& shell,
                                                         const bool is_popup, const bool is_devtools)
    : runtime_(runtime), shell_(shell), is_popup_(is_popup), is_devtools_(is_devtools) {}

void CefViewsBrowserViewDelegate::OnBrowserDestroyed(CefRefPtr<CefBrowserView> browser_view, CefRefPtr<CefBrowser>) {
    if (browser_view == nullptr) {
        return;
    }

    CefRefPtr<CefWindow> window = browser_view->GetWindow();
    if (window != nullptr && !window->IsClosed()) {
        window->Close();
    }
}

CefRefPtr<CefBrowserViewDelegate> CefViewsBrowserViewDelegate::GetDelegateForPopupBrowserView(CefRefPtr<CefBrowserView>,
                                                                                              const CefBrowserSettings&,
                                                                                              CefRefPtr<CefClient>,
                                                                                              const bool is_devtools) {
    return new CefViewsBrowserViewDelegate(runtime_, shell_, true, is_devtools);
}

bool CefViewsBrowserViewDelegate::OnPopupBrowserViewCreated(CefRefPtr<CefBrowserView>,
                                                            CefRefPtr<CefBrowserView> popup_browser_view,
                                                            const bool is_devtools) {
    shell_.create_popup_window(popup_browser_view, is_devtools_ || is_popup_ || is_devtools);
    return true;
}

cef_runtime_style_t CefViewsBrowserViewDelegate::GetBrowserRuntimeStyle() {
    return CEF_RUNTIME_STYLE_ALLOY;
}

}  // namespace mmltk::gui
