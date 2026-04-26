#include "gui/cef_subprocess_app.h"

#include "include/cef_app.h"

int main(int argc, char* argv[]) {
    CefMainArgs main_args(argc, argv);
    CefRefPtr<mmltk::gui::BrowserSubprocessApp> app = new mmltk::gui::BrowserSubprocessApp();
    return CefExecuteProcess(main_args, app, nullptr);
}
