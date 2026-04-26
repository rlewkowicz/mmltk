#pragma once

#include "include/views/cef_browser_view.h"

namespace mmltk::gui {

class EmbeddedCefBrowserRuntime;

class CefBrowserShell {
   public:
    explicit CefBrowserShell(EmbeddedCefBrowserRuntime& runtime);

    void create_main_window();
    void create_popup_window(const CefRefPtr<CefBrowserView>& browser_view, bool is_devtools);

   private:
    EmbeddedCefBrowserRuntime& runtime_;
};

}  // namespace mmltk::gui
