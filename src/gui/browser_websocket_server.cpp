#include "gui/browser_websocket_server.h"

#include "mmltk_logging.h"

#include <App.h>
#include <libusockets.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::gui {

namespace {

struct BrowserWebSocketPeer {
    bool subscribed = true;
};

using BrowserWebSocket = uWS::WebSocket<false, true, BrowserWebSocketPeer>;

[[nodiscard]] std::string websocket_error_message(const std::string& message) {
    return nlohmann::json({{"type", "bridge.error"}, {"message", message}}).dump();
}

}  // namespace

struct BrowserWebSocketServer::Impl {
    [[nodiscard]] bool start(Callbacks next_callbacks) {
        std::unique_lock lock(mutex);
        if (running) {
            return true;
        }

        callbacks = std::move(next_callbacks);
        stop_requested = false;
        startup_complete = false;
        startup_ok = false;
        startup_port = 0;
        listen_socket = nullptr;
        loop = nullptr;

        worker = std::thread([this] { run_loop(); });
        startup_cv.wait(lock, [this] { return startup_complete; });
        if (!startup_ok) {
            lock.unlock();
            stop();
            return false;
        }
        running = true;
        return true;
    }

    void stop() {
        uWS::Loop* loop_to_stop = nullptr;
        {
            std::scoped_lock lock(mutex);
            if (stop_requested) {
                return;
            }
            stop_requested = true;
            loop_to_stop = loop;
        }

        if (loop_to_stop != nullptr) {
            loop_to_stop->defer([this] {
                if (listen_socket != nullptr) {
                    us_listen_socket_close(0, listen_socket);
                    listen_socket = nullptr;
                }
                std::vector<BrowserWebSocket*> peers;
                peers.reserve(sockets.size());
                for (BrowserWebSocket* socket : sockets) {
                    peers.push_back(socket);
                }
                for (BrowserWebSocket* socket : peers) {
                    socket->close();
                }
            });
        }

        if (worker.joinable()) {
            worker.join();
        }

        std::scoped_lock lock(mutex);
        running = false;
        loop = nullptr;
        listen_socket = nullptr;
        startup_complete = false;
        startup_ok = false;
        startup_port = 0;
    }

    [[nodiscard]] std::optional<std::string> url() const {
        std::scoped_lock lock(mutex);
        if (!running || startup_port <= 0) {
            return std::nullopt;
        }
        return "ws://127.0.0.1:" + std::to_string(startup_port) + "/mmltk";
    }

    void publish_bridge_state(std::string message) {
        {
            std::scoped_lock lock(mutex);
            latest_bridge_state = message;
        }
        broadcast(std::move(message));
    }

    void publish_snapshot(std::string message) {
        {
            std::scoped_lock lock(mutex);
            latest_snapshot = message;
        }
        broadcast(std::move(message));
    }

    void publish_surface_ready(std::string message) {
        {
            std::scoped_lock lock(mutex);
            latest_surface_ready = message;
        }
        broadcast(std::move(message));
    }

    void publish_error(const std::string& message) {
        broadcast(websocket_error_message(message));
    }

    void broadcast(std::string message) {
        uWS::Loop* loop_for_send = nullptr;
        {
            std::scoped_lock lock(mutex);
            if (!running || stop_requested || loop == nullptr) {
                return;
            }
            loop_for_send = loop;
        }
        loop_for_send->defer([this, message = std::move(message)]() mutable {
            for (BrowserWebSocket* socket : sockets) {
                if (socket->getUserData()->subscribed) {
                    socket->send(message, uWS::OpCode::TEXT, false);
                }
            }
        });
    }

    void run_loop() {
        uWS::App app;
        {
            std::scoped_lock lock(mutex);
            loop = uWS::Loop::get();
        }

        app.ws<BrowserWebSocketPeer>(
               "/mmltk", {.compression = uWS::DISABLED,
                          .maxPayloadLength = 1024 * 1024,
                          .idleTimeout = 64,
                          .open =
                              [this](BrowserWebSocket* socket) {
                                  sockets.insert(socket);
                                  std::string bridge_state;
                                  std::string snapshot;
                                  std::string surface_ready;
                                  {
                                      std::scoped_lock lock(mutex);
                                      bridge_state = latest_bridge_state;
                                      snapshot = latest_snapshot;
                                      surface_ready = latest_surface_ready;
                                  }
                                  if (!bridge_state.empty()) {
                                      socket->send(bridge_state, uWS::OpCode::TEXT, false);
                                  }
                                  if (!snapshot.empty()) {
                                      socket->send(snapshot, uWS::OpCode::TEXT, false);
                                  }
                                  if (!surface_ready.empty()) {
                                      socket->send(surface_ready, uWS::OpCode::TEXT, false);
                                  }
                              },
                          .message =
                              [this](BrowserWebSocket*, std::string_view message, uWS::OpCode opcode) {
                                  if (opcode != uWS::OpCode::TEXT || message.empty()) {
                                      return;
                                  }
                                  std::function<void(std::string)> handler;
                                  {
                                      std::scoped_lock lock(mutex);
                                      handler = callbacks.on_message;
                                  }
                                  if (handler) {
                                      handler(std::string(message));
                                  }
                              },
                          .close = [this](BrowserWebSocket* socket, int, std::string_view) { sockets.erase(socket); }})
            .get("/health",
                 [](auto* response, auto*) {
                     response->writeHeader("Content-Type", "text/plain");
                     response->end("ok");
                 })
            .listen("127.0.0.1", 0, [this](us_listen_socket_t* token) {
                std::scoped_lock lock(mutex);
                listen_socket = token;
                startup_ok = token != nullptr;
                startup_port = token != nullptr ? us_socket_local_port(0, reinterpret_cast<us_socket_t*>(token)) : 0;
                startup_complete = true;
                startup_cv.notify_all();
            });

        {
            std::scoped_lock lock(mutex);
            if (!startup_complete) {
                startup_ok = false;
                startup_complete = true;
                startup_cv.notify_all();
            }
        }

        if (startup_ok) {
            mmltk::logging::logger("gui")->info(
                "embedded browser WebSocket transport listening on ws://127.0.0.1:{}/mmltk", startup_port);
            app.run();
        }

        sockets.clear();
        {
            std::scoped_lock lock(mutex);
            running = false;
            listen_socket = nullptr;
            loop = nullptr;
        }
    }

    mutable std::mutex mutex;
    std::condition_variable startup_cv;
    Callbacks callbacks;
    std::thread worker;
    uWS::Loop* loop = nullptr;
    us_listen_socket_t* listen_socket = nullptr;
    std::unordered_set<BrowserWebSocket*> sockets;
    std::string latest_bridge_state;
    std::string latest_snapshot;
    std::string latest_surface_ready;
    int startup_port = 0;
    bool running = false;
    bool stop_requested = false;
    bool startup_complete = false;
    bool startup_ok = false;
};

BrowserWebSocketServer::BrowserWebSocketServer() : impl_(std::make_unique<Impl>()) {}

BrowserWebSocketServer::~BrowserWebSocketServer() {
    stop();
}

bool BrowserWebSocketServer::start(Callbacks callbacks) {
    return impl_->start(std::move(callbacks));
}

void BrowserWebSocketServer::stop() {
    impl_->stop();
}

std::optional<std::string> BrowserWebSocketServer::url() const {
    return impl_->url();
}

void BrowserWebSocketServer::publish_bridge_state(std::string message) {
    impl_->publish_bridge_state(std::move(message));
}

void BrowserWebSocketServer::publish_snapshot(std::string message) {
    impl_->publish_snapshot(std::move(message));
}

void BrowserWebSocketServer::publish_surface_ready(std::string message) {
    impl_->publish_surface_ready(std::move(message));
}

void BrowserWebSocketServer::publish_error(const std::string& message) {
    impl_->publish_error(message);
}

}  // namespace mmltk::gui
