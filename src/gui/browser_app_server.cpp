#include "gui/browser_app_server.h"

#include "mmltk_logging.h"

#include <App.h>
#include <libusockets.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::gui {

namespace {

constexpr std::size_t kMaxWebSocketPayloadBytes = std::size_t{1024U} * 1024U;
constexpr std::string_view kCallbacksReadyMessageType = "host.callbacks.ready";
constexpr std::string_view kRuntimeCapabilitiesMessageType = "host.runtime.capabilities";
constexpr std::string_view kWorkspaceReleaseMessageType = "workspace.release";

struct BrowserAppPeer {
    std::uint64_t bridge_generation = 0U;
    std::uint64_t snapshot_generation = 0U;
    std::uint64_t workspace_generation = 0U;
    std::uint64_t connection_epoch = 0U;
};

using BrowserSocket = uWS::WebSocket<false, true, BrowserAppPeer>;

struct BrowserAsset {
    std::string url;
    std::string mime_type;
    std::string content;
    bool immutable = false;
};

struct QueuedInboundMessage {
    BrowserAppInboundMessage message;
    std::size_t wire_bytes = 0U;
};

[[nodiscard]] bool constant_time_equal(const std::string_view left, const std::string_view right) noexcept {
    std::size_t difference = left.size() ^ right.size();
    const std::size_t common = std::min(left.size(), right.size());
    for (std::size_t index = 0U; index < common; ++index) {
        difference |= static_cast<unsigned char>(left[index]) ^ static_cast<unsigned char>(right[index]);
    }
    return difference == 0U;
}

[[nodiscard]] std::string mime_type_for_path(const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    if (extension == ".html") {
        return "text/html; charset=utf-8";
    }
    if (extension == ".js" || extension == ".mjs") {
        return "text/javascript; charset=utf-8";
    }
    if (extension == ".css") {
        return "text/css; charset=utf-8";
    }
    if (extension == ".wasm") {
        return "application/wasm";
    }
    if (extension == ".json") {
        return "application/json; charset=utf-8";
    }
    if (extension == ".svg") {
        return "image/svg+xml";
    }
    if (extension == ".png") {
        return "image/png";
    }
    if (extension == ".webp") {
        return "image/webp";
    }
    if (extension == ".woff2") {
        return "font/woff2";
    }
    if (extension == ".ico") {
        return "image/x-icon";
    }
    return "application/octet-stream";
}

[[nodiscard]] std::string read_asset(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw std::runtime_error("failed to open browser asset: " + path.string());
    }
    const std::streamoff end = stream.tellg();
    if (end < 0) {
        throw std::runtime_error("failed to size browser asset: " + path.string());
    }
    if (static_cast<std::uintmax_t>(end) > static_cast<std::uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error("browser asset is too large: " + path.string());
    }
    std::string content(static_cast<std::size_t>(end), '\0');
    stream.seekg(0, std::ios::beg);
    if (!content.empty() && !stream.read(content.data(), static_cast<std::streamsize>(content.size()))) {
        throw std::runtime_error("failed to read browser asset: " + path.string());
    }
    return content;
}

[[nodiscard]] std::vector<BrowserAsset> load_assets(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root) || !std::filesystem::is_regular_file(root / "index.html")) {
        throw std::runtime_error("browser asset root does not contain index.html: " + root.string());
    }

    std::vector<BrowserAsset> assets;
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_symlink() || !entry.is_regular_file()) {
            continue;
        }
        const std::filesystem::path relative = entry.path().lexically_relative(root);
        if (relative.empty() || relative.is_absolute()) {
            continue;
        }
        const std::string relative_url = relative.generic_string();
        BrowserAsset asset;
        asset.url.reserve(relative_url.size() + 1U);
        asset.url.push_back('/');
        asset.url.append(relative_url);
        asset.mime_type = mime_type_for_path(entry.path());
        asset.content = read_asset(entry.path());
        asset.immutable = relative.filename() != "index.html";
        assets.push_back(std::move(asset));
    }
    std::sort(assets.begin(), assets.end(),
              [](const BrowserAsset& left, const BrowserAsset& right) { return left.url < right.url; });
    return assets;
}

[[nodiscard]] std::string websocket_error_message(const std::string_view message) {
    return nlohmann::json{{"type", "bridge.error"}, {"message", message}}.dump();
}

[[nodiscard]] mmltk::browser::BrowserRuntimeCapabilityStatus parse_navigator_gpu(const nlohmann::json& message) {
    const auto found = message.find("navigator_gpu");
    if (found == message.end() || found->is_null()) {
        return mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
    }
    if (found->is_boolean()) {
        return found->get<bool>() ? mmltk::browser::BrowserRuntimeCapabilityStatus::Available
                                  : mmltk::browser::BrowserRuntimeCapabilityStatus::Unavailable;
    }
    if (found->is_string()) {
        return mmltk::browser::browser_runtime_capability_status_from_name(found->get<std::string>());
    }
    throw std::runtime_error("navigator_gpu capability must be a boolean or status string");
}

[[nodiscard]] BrowserAppInboundMessage parse_inbound_message(const std::string_view payload) {
    BrowserAppInboundMessage inbound_message;
    try {
        const nlohmann::json message = nlohmann::json::parse(payload);
        const std::string type = message.at("type").get<std::string>();
        if (type == mmltk::browser::kIntentMessageType) {
            inbound_message.kind = BrowserAppInboundMessage::Kind::Intent;
            inbound_message.intent = message.get<mmltk::browser::IntentMessage>();
            return inbound_message;
        }
        if (type == kCallbacksReadyMessageType) {
            inbound_message.kind = BrowserAppInboundMessage::Kind::CallbacksReady;
            return inbound_message;
        }
        if (type == kRuntimeCapabilitiesMessageType) {
            inbound_message.kind = BrowserAppInboundMessage::Kind::RuntimeCapabilities;
            inbound_message.navigator_gpu = parse_navigator_gpu(message);
            if (const auto capabilities = message.find("capabilities");
                capabilities != message.end() && capabilities->is_object()) {
                inbound_message.capabilities = *capabilities;
            }
            return inbound_message;
        }
        if (type == kWorkspaceReleaseMessageType) {
            inbound_message.kind = BrowserAppInboundMessage::Kind::WorkspaceRelease;
            inbound_message.workspace_generation = std::stoull(message.at("generation").get<std::string>());
            inbound_message.workspace_revision = std::stoull(message.at("revision").get<std::string>());
            inbound_message.workspace_slot = message.at("slot").get<std::uint32_t>();
            inbound_message.workspace_encoded = message.at("encoded").get<bool>();
            return inbound_message;
        }
        throw std::runtime_error("unsupported browser message type: " + type);
    } catch (const std::exception& error) {
        inbound_message.kind = BrowserAppInboundMessage::Kind::Rejected;
        inbound_message.error = error.what();
        return inbound_message;
    }
}

[[nodiscard]] std::string percent_encode_query_value(const std::string_view value) {
    constexpr std::string_view hex = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value) {
        const bool unreserved = (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' ||
                                byte == '~';
        if (unreserved) {
            encoded.push_back(static_cast<char>(byte));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[byte >> 4U]);
            encoded.push_back(hex[byte & 0x0fU]);
        }
    }
    return encoded;
}

}  

struct BrowserAppServer::Impl {
    [[nodiscard]] bool start(Config next_config, Callbacks next_callbacks) {
        std::unique_lock lock(mutex);
        if (running) {
            return true;
        }
        if (next_config.session_token.empty() || next_config.max_inbound_messages == 0U ||
            next_config.max_inbound_bytes == 0U || next_config.max_control_messages == 0U ||
            next_config.max_control_bytes == 0U || next_config.max_outbound_message_bytes == 0U ||
            next_config.max_outbound_message_bytes > std::numeric_limits<unsigned int>::max() ||
            next_config.max_outbound_bytes > std::numeric_limits<unsigned int>::max() ||
            next_config.max_outbound_bytes < next_config.max_outbound_message_bytes ||
            next_config.max_control_bytes > next_config.max_outbound_bytes) {
            error_text = "browser app server requires a session token and valid nonzero queue limits";
            return false;
        }
        try {
            assets = load_assets(next_config.asset_root);
        } catch (const std::exception& error) {
            error_text = error.what();
            return false;
        }

        config = std::move(next_config);
        callbacks = std::move(next_callbacks);
        error_text.clear();
        stop_requested = false;
        startup_complete = false;
        startup_ok = false;
        startup_port = 0;
        listen_socket = nullptr;
        loop = nullptr;
        active_socket = nullptr;
        inbound.clear();
        inbound_bytes = 0U;
        controls.clear();
        control_bytes = 0U;
        latest_bridge_state.clear();
        latest_snapshot.clear();
        latest_workspace_present.clear();
        workspace_displaced_callback = {};
        bridge_generation = 0U;
        snapshot_generation = 0U;
        workspace_generation = 0U;
        outbound_scheduled = false;
        connected_flag.store(false, std::memory_order_release);
        connection_epoch_counter.store(0U, std::memory_order_release);

        worker = std::thread([this] { run_loop(); });
        startup_cv.wait(lock, [this] { return startup_complete; });
        if (!startup_ok) {
            lock.unlock();
            if (worker.joinable()) {
                worker.join();
            }
            lock.lock();
            stop_requested = true;
            running = false;
            loop = nullptr;
            listen_socket = nullptr;
            active_socket = nullptr;
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
                if (active_socket != nullptr) {
                    active_socket->close();
                    active_socket = nullptr;
                }
            });
        }
        if (worker.joinable()) {
            worker.join();
        }
        std::function<void()> displaced;
        {
            std::scoped_lock lock(mutex);
            running = false;
            loop = nullptr;
            listen_socket = nullptr;
            active_socket = nullptr;
            startup_complete = false;
            startup_ok = false;
            startup_port = 0;
            connected_flag.store(false, std::memory_order_release);
            displaced = std::move(workspace_displaced_callback);
        }
        if (displaced) {
            displaced();
        }
    }

    [[nodiscard]] std::optional<std::string> page_url() const {
        std::scoped_lock lock(mutex);
        if (!running || startup_port <= 0) {
            return std::nullopt;
        }
        const std::string socket_url =
            "ws://127.0.0.1:" + std::to_string(startup_port) + "/mmltk?session=" + config.session_token;
        std::string url = expected_origin + "/?session=" + config.session_token +
                          "&mmltk_ws_url=" + percent_encode_query_value(socket_url);
        if (config.ui_trace_enabled) {
            url.append("&mmltk_trace=1");
        }
        return url;
    }

    [[nodiscard]] std::optional<std::string> websocket_url() const {
        std::scoped_lock lock(mutex);
        if (!running || startup_port <= 0) {
            return std::nullopt;
        }
        return "ws://127.0.0.1:" + std::to_string(startup_port) + "/mmltk?session=" + config.session_token;
    }

    [[nodiscard]] std::string last_error() const {
        std::scoped_lock lock(mutex);
        return error_text;
    }

    void drain_inbound(std::vector<BrowserAppInboundMessage>& messages, const std::size_t max_messages) {
        messages.clear();
        std::scoped_lock lock(mutex);
        const std::size_t count = std::min(max_messages, inbound.size());
        if (messages.capacity() < count) {
            messages.reserve(count);
        }
        for (std::size_t index = 0U; index < count; ++index) {
            inbound_bytes -= inbound.front().wire_bytes;
            messages.push_back(std::move(inbound.front().message));
            inbound.pop_front();
        }
    }

    [[nodiscard]] bool replace_latest_outbound(std::string& destination, std::string message,
                                               const std::string_view label) {
        if (message.size() > config.max_outbound_message_bytes) {
            error_text = std::string(label) + " exceeds the browser outbound message limit";
            return false;
        }
        const std::size_t retained_bytes =
            latest_bridge_state.size() + latest_snapshot.size() + latest_workspace_present.size() - destination.size();
        while (!controls.empty() &&
               message.size() >
                   config.max_outbound_bytes - std::min(retained_bytes + control_bytes, config.max_outbound_bytes)) {
            control_bytes -= controls.front().size();
            controls.pop_front();
        }
        if (message.size() >
            config.max_outbound_bytes - std::min(retained_bytes + control_bytes, config.max_outbound_bytes)) {
            error_text = std::string(label) + " exceeds the browser outbound queue limit";
            return false;
        }
        destination = std::move(message);
        return true;
    }

    [[nodiscard]] bool publish_bridge_state(std::string message) {
        {
            std::scoped_lock lock(mutex);
            if (!replace_latest_outbound(latest_bridge_state, std::move(message), "bridge state")) {
                return false;
            }
            ++bridge_generation;
        }
        schedule_outbound();
        return true;
    }

    [[nodiscard]] bool publish_snapshot(std::string message) {
        {
            std::scoped_lock lock(mutex);
            if (!replace_latest_outbound(latest_snapshot, std::move(message), "state snapshot")) {
                return false;
            }
            ++snapshot_generation;
        }
        schedule_outbound();
        return true;
    }

    [[nodiscard]] bool publish_workspace_present(std::string message, std::function<void()> on_displaced) {
        std::function<void()> displaced;
        {
            std::scoped_lock lock(mutex);
            if (!replace_latest_outbound(latest_workspace_present, std::move(message), "workspace present")) {
                return false;
            }
            displaced = std::move(workspace_displaced_callback);
            workspace_displaced_callback = std::move(on_displaced);
            ++workspace_generation;
        }
        if (displaced) {
            displaced();
        }
        schedule_outbound();
        return true;
    }

    [[nodiscard]] bool publish_error(const std::string& message) {
        std::string encoded = websocket_error_message(message);
        {
            std::scoped_lock lock(mutex);
            if (encoded.size() > config.max_outbound_message_bytes || encoded.size() > config.max_control_bytes) {
                error_text = "bridge error exceeds the browser outbound control limit";
                return false;
            }
            while (!controls.empty() &&
                   (controls.size() >= config.max_control_messages ||
                    encoded.size() > config.max_control_bytes - std::min(control_bytes, config.max_control_bytes) ||
                    encoded.size() >
                        config.max_outbound_bytes - std::min(latest_bridge_state.size() + latest_snapshot.size() +
                                                                 latest_workspace_present.size() + control_bytes,
                                                             config.max_outbound_bytes))) {
                control_bytes -= controls.front().size();
                controls.pop_front();
            }
            const std::size_t retained_bytes =
                latest_bridge_state.size() + latest_snapshot.size() + latest_workspace_present.size() + control_bytes;
            if (encoded.size() > config.max_control_bytes - std::min(control_bytes, config.max_control_bytes) ||
                encoded.size() > config.max_outbound_bytes - std::min(retained_bytes, config.max_outbound_bytes)) {
                error_text = "bridge error exceeds the browser outbound queue limit";
                return false;
            }
            control_bytes += encoded.size();
            controls.push_back(std::move(encoded));
        }
        schedule_outbound();
        return true;
    }

    void schedule_outbound() {
        uWS::Loop* target_loop = nullptr;
        {
            std::scoped_lock lock(mutex);
            if (!running || stop_requested || loop == nullptr || outbound_scheduled) {
                return;
            }
            outbound_scheduled = true;
            target_loop = loop;
        }
        target_loop->defer([this] { flush_outbound(); });
    }

    void flush_outbound() {
        std::string bridge;
        std::string snapshot;
        std::string workspace_present;
        std::function<void()> displaced_workspace;
        std::deque<std::string> pending_controls;
        std::uint64_t next_bridge_generation = 0U;
        std::uint64_t next_snapshot_generation = 0U;
        std::uint64_t next_workspace_generation = 0U;
        {
            std::scoped_lock lock(mutex);
            outbound_scheduled = false;
            if (active_socket == nullptr) {
                return;
            }
            if (active_socket->getUserData()->bridge_generation != bridge_generation) {
                bridge = latest_bridge_state;
                next_bridge_generation = bridge_generation;
            }
            if (active_socket->getUserData()->snapshot_generation != snapshot_generation) {
                snapshot = latest_snapshot;
                next_snapshot_generation = snapshot_generation;
            }
            if (active_socket->getUserData()->workspace_generation != workspace_generation) {
                workspace_present = latest_workspace_present;
                displaced_workspace = std::move(workspace_displaced_callback);
                next_workspace_generation = workspace_generation;
            }
            pending_controls.swap(controls);
            control_bytes = 0U;
        }

        const auto requeue_or_release_workspace = [&] {
            if (!displaced_workspace) {
                return;
            }
            std::function<void()> release;
            {
                std::scoped_lock lock(mutex);
                if (workspace_generation == next_workspace_generation && !workspace_displaced_callback) {
                    workspace_displaced_callback = std::move(displaced_workspace);
                } else {
                    release = std::move(displaced_workspace);
                }
            }
            if (release) {
                release();
            }
        };
        BrowserAppPeer* peer = active_socket != nullptr ? active_socket->getUserData() : nullptr;
        if (peer == nullptr) {
            requeue_or_release_workspace();
            return;
        }
        const auto requeue_controls = [&](std::deque<std::string>& unsent) {
            std::scoped_lock lock(mutex);
            while (!unsent.empty()) {
                std::string message = std::move(unsent.back());
                unsent.pop_back();
                const std::size_t retained_bytes = latest_bridge_state.size() + latest_snapshot.size() +
                                                   latest_workspace_present.size() + control_bytes;
                if (controls.size() >= config.max_control_messages ||
                    message.size() > config.max_control_bytes - std::min(control_bytes, config.max_control_bytes) ||
                    message.size() > config.max_outbound_bytes - std::min(retained_bytes, config.max_outbound_bytes)) {
                    continue;
                }
                control_bytes += message.size();
                controls.push_front(std::move(message));
            }
        };
        const auto send_bounded = [&](const std::string_view message) {
            const std::size_t buffered = active_socket->getBufferedAmount();
            if (message.size() > config.max_outbound_bytes - std::min(buffered, config.max_outbound_bytes)) {
                return false;
            }
            return active_socket->send(message, uWS::OpCode::TEXT, false) != BrowserSocket::SendStatus::DROPPED;
        };
        if (!bridge.empty()) {
            if (!send_bounded(bridge)) {
                requeue_or_release_workspace();
                requeue_controls(pending_controls);
                return;
            }
            peer->bridge_generation = next_bridge_generation;
        }
        if (!snapshot.empty()) {
            if (!send_bounded(snapshot)) {
                requeue_or_release_workspace();
                requeue_controls(pending_controls);
                return;
            }
            peer->snapshot_generation = next_snapshot_generation;
        }
        if (!workspace_present.empty()) {
            if (!send_bounded(workspace_present)) {
                requeue_or_release_workspace();
                requeue_controls(pending_controls);
                return;
            }
            peer->workspace_generation = next_workspace_generation;
            displaced_workspace = {};
        }
        while (!pending_controls.empty()) {
            if (!send_bounded(pending_controls.front())) {
                requeue_controls(pending_controls);
                return;
            }
            pending_controls.pop_front();
        }
    }

    [[nodiscard]] const BrowserAsset* find_asset(const std::string_view url) const noexcept {
        const std::string_view normalized = url == "/" ? std::string_view{"/index.html"} : url;
        if (normalized.empty() || normalized.find("..") != std::string_view::npos ||
            normalized.find('\\') != std::string_view::npos || normalized.find('%') != std::string_view::npos) {
            return nullptr;
        }
        const auto found = std::lower_bound(
            assets.begin(), assets.end(), normalized,
            [](const BrowserAsset& asset, const std::string_view needle) { return asset.url < needle; });
        return found != assets.end() && found->url == normalized ? &*found : nullptr;
    }

    void serve_asset(uWS::HttpResponse<false>* response, uWS::HttpRequest* request) const {
        const BrowserAsset* asset = find_asset(request->getUrl());
        if (asset == nullptr) {
            response->writeStatus("404 Not Found")->end("not found");
            return;
        }
        response->writeHeader("Content-Type", asset->mime_type)
            ->writeHeader("Cache-Control", asset->immutable ? "public, max-age=31536000, immutable" : "no-store")
            ->writeHeader("Cross-Origin-Opener-Policy", "same-origin")
            ->writeHeader("Cross-Origin-Embedder-Policy", "require-corp")
            ->writeHeader("Content-Security-Policy",
                          "default-src 'self'; connect-src 'self' ws://127.0.0.1:*; script-src 'self' "
                          "'unsafe-inline' 'wasm-unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' "
                          "data:; font-src 'self' data:")
            ->writeHeader("X-Content-Type-Options", "nosniff")
            ->end(asset->content);
    }

    void enqueue_inbound(BrowserSocket* socket, const std::string_view message) {
        BrowserAppInboundMessage parsed = parse_inbound_message(message);
        parsed.connection_epoch = socket->getUserData()->connection_epoch;
        bool queue_full = false;
        {
            std::scoped_lock lock(mutex);
            if (inbound.size() >= config.max_inbound_messages ||
                message.size() > config.max_inbound_bytes - std::min(inbound_bytes, config.max_inbound_bytes)) {
                queue_full = true;
            } else {
                inbound.push_back(QueuedInboundMessage{std::move(parsed), message.size()});
                inbound_bytes += message.size();
            }
        }
        if (queue_full) {
            socket->end(1009, "inbound queue full");
            return;
        }
        if (callbacks.on_inbound_ready) {
            callbacks.on_inbound_ready();
        }
    }

    void notify_connection_changed() {
        if (callbacks.on_connection_changed) {
            callbacks.on_connection_changed();
        }
    }

    void run_loop() {
        uWS::App app;
        {
            std::scoped_lock lock(mutex);
            loop = uWS::Loop::get();
        }

        app.ws<BrowserAppPeer>(
               "/mmltk",
               {.compression = uWS::DISABLED,
                .maxPayloadLength = static_cast<unsigned int>(kMaxWebSocketPayloadBytes),
                .idleTimeout = 64,
                .maxBackpressure = static_cast<unsigned int>(config.max_outbound_bytes),
                .closeOnBackpressureLimit = true,
                .upgrade =
                    [this](uWS::HttpResponse<false>* response, uWS::HttpRequest* request,
                           us_socket_context_t* context) {
                        const std::string_view session = request->getQuery("session");
                        const std::string_view origin = request->getHeader("origin");
                        if (!constant_time_equal(session, config.session_token) || origin != expected_origin) {
                            response->writeStatus("403 Forbidden")->end("forbidden");
                            return;
                        }
                        response->upgrade<BrowserAppPeer>({}, request->getHeader("sec-websocket-key"),
                                                          request->getHeader("sec-websocket-protocol"),
                                                          request->getHeader("sec-websocket-extensions"), context);
                    },
                .open =
                    [this](BrowserSocket* socket) {
                        if (active_socket != nullptr && active_socket != socket) {
                            active_socket->end(1008, "superseded");
                        }
                        const std::uint64_t connection_epoch =
                            connection_epoch_counter.fetch_add(1U, std::memory_order_acq_rel) + 1U;
                        socket->getUserData()->connection_epoch = connection_epoch;
                        {
                            std::scoped_lock lock(mutex);
                            inbound.clear();
                            inbound_bytes = 0U;
                        }
                        active_socket = socket;
                        connected_flag.store(true, std::memory_order_release);
                        schedule_outbound();
                        notify_connection_changed();
                    },
                .message =
                    [this](BrowserSocket* socket, const std::string_view message, const uWS::OpCode opcode) {
                        if (socket != active_socket) {
                            return;
                        }
                        if (opcode == uWS::OpCode::TEXT && !message.empty()) {
                            enqueue_inbound(socket, message);
                        } else if (opcode != uWS::OpCode::TEXT) {
                            socket->end(1003, "text messages required");
                        }
                    },
                .drain =
                    [this](BrowserSocket* socket) {
                        if (active_socket == socket) {
                            schedule_outbound();
                        }
                    },
                .close =
                    [this](BrowserSocket* socket, int, std::string_view) {
                        if (active_socket == socket) {
                            active_socket = nullptr;
                            connected_flag.store(false, std::memory_order_release);
                            notify_connection_changed();
                        }
                    }})
            .get("/health",
                 [](uWS::HttpResponse<false>* response, uWS::HttpRequest*) {
                     response->writeHeader("Content-Type", "text/plain; charset=utf-8")
                         ->writeHeader("Cache-Control", "no-store")
                         ->end("ok");
                 })
            .get("/*", [this](uWS::HttpResponse<false>* response,
                              uWS::HttpRequest* request) { serve_asset(response, request); })
            .listen("127.0.0.1", 0, [this](us_listen_socket_t* token) {
                std::scoped_lock lock(mutex);
                listen_socket = token;
                startup_ok = token != nullptr;
                startup_port = token != nullptr ? us_socket_local_port(0, reinterpret_cast<us_socket_t*>(token)) : 0;
                expected_origin = startup_ok ? "http://127.0.0.1:" + std::to_string(startup_port) : std::string{};
                if (!startup_ok) {
                    error_text = "failed to bind browser app server to loopback";
                }
                startup_complete = true;
                startup_cv.notify_all();
            });

        {
            std::scoped_lock lock(mutex);
            if (!startup_complete) {
                startup_ok = false;
                error_text = "browser app server did not complete startup";
                startup_complete = true;
                startup_cv.notify_all();
            }
        }
        if (startup_ok) {
            mmltk::logging::logger("gui")->debug("browser app server listening on {}", expected_origin);
            app.run();
        }
        connected_flag.store(false, std::memory_order_release);
        {
            std::scoped_lock lock(mutex);
            running = false;
            active_socket = nullptr;
            listen_socket = nullptr;
            loop = nullptr;
        }
    }

    Config config;
    Callbacks callbacks;
    std::vector<BrowserAsset> assets;
    mutable std::mutex mutex;
    std::condition_variable startup_cv;
    std::thread worker;
    uWS::Loop* loop = nullptr;
    us_listen_socket_t* listen_socket = nullptr;
    BrowserSocket* active_socket = nullptr;
    std::deque<QueuedInboundMessage> inbound;
    std::size_t inbound_bytes = 0U;
    std::deque<std::string> controls;
    std::size_t control_bytes = 0U;
    std::string latest_bridge_state;
    std::string latest_snapshot;
    std::string latest_workspace_present;
    std::function<void()> workspace_displaced_callback;
    std::string expected_origin;
    std::string error_text;
    std::uint64_t bridge_generation = 0U;
    std::uint64_t snapshot_generation = 0U;
    std::uint64_t workspace_generation = 0U;
    int startup_port = 0;
    bool running = false;
    bool stop_requested = false;
    bool startup_complete = false;
    bool startup_ok = false;
    bool outbound_scheduled = false;
    std::atomic_bool connected_flag{false};
    std::atomic<std::uint64_t> connection_epoch_counter{0U};
};

BrowserAppServer::BrowserAppServer() : impl_(std::make_unique<Impl>()) {}

BrowserAppServer::~BrowserAppServer() {
    stop();
}

bool BrowserAppServer::start(Config config, Callbacks callbacks) {
    return impl_->start(std::move(config), std::move(callbacks));
}

void BrowserAppServer::stop() {
    impl_->stop();
}

std::optional<std::string> BrowserAppServer::page_url() const {
    return impl_->page_url();
}

std::optional<std::string> BrowserAppServer::websocket_url() const {
    return impl_->websocket_url();
}

std::string BrowserAppServer::last_error() const {
    return impl_->last_error();
}

bool BrowserAppServer::connected() const noexcept {
    return impl_->connected_flag.load(std::memory_order_acquire);
}

std::uint64_t BrowserAppServer::connection_epoch() const noexcept {
    return impl_->connection_epoch_counter.load(std::memory_order_acquire);
}

void BrowserAppServer::drain_inbound(std::vector<BrowserAppInboundMessage>& messages, const std::size_t max_messages) {
    impl_->drain_inbound(messages, max_messages);
}

bool BrowserAppServer::publish_bridge_state(std::string message) {
    return impl_->publish_bridge_state(std::move(message));
}

bool BrowserAppServer::publish_snapshot(std::string message) {
    return impl_->publish_snapshot(std::move(message));
}

bool BrowserAppServer::publish_workspace_present(std::string message, std::function<void()> on_displaced) {
    return impl_->publish_workspace_present(std::move(message), std::move(on_displaced));
}

bool BrowserAppServer::publish_error(const std::string& message) {
    return impl_->publish_error(message);
}

}  
