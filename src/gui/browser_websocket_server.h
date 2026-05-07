#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace mmltk::gui {

class BrowserWebSocketServer {
   public:
    struct Callbacks {
        std::function<void(std::string)> on_message;
    };

    BrowserWebSocketServer();
    ~BrowserWebSocketServer();

    BrowserWebSocketServer(const BrowserWebSocketServer&) = delete;
    BrowserWebSocketServer& operator=(const BrowserWebSocketServer&) = delete;

    [[nodiscard]] bool start(Callbacks callbacks);
    void stop();

    [[nodiscard]] std::optional<std::string> url() const;

    void publish_bridge_state(std::string message);
    void publish_snapshot(std::string message);
    void publish_surface_ready(std::string message);
    void publish_error(const std::string& message);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::gui
