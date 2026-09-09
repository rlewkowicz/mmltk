#include "src/frameworks/transport/browser_server.h"

#include <App.h>
#include <libusockets.h>

#include <atomic>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "src/common/types/string_utils.h"
#include "src/frameworks/transport/browser_server_lifecycle.h"

namespace mmltk::frameworks::transport {
namespace {

inline constexpr std::size_t kMaximumMessageBytes = 8U * 1024U * 1024U;

struct Peer final {
    detail::BrowserPeerLifecycle lifecycle;
};
using Socket = uWS::WebSocket<false, true, Peer>;

struct Asset final {
    std::string path;
    std::string mime;
    std::string bytes;
    bool immutable = false;
};

[[nodiscard]] std::string mime_for(const std::filesystem::path& path) {
    const auto extension = path.extension().string();
    if (extension == ".html") return "text/html; charset=utf-8";
    if (extension == ".js" || extension == ".mjs") return "text/javascript; charset=utf-8";
    if (extension == ".css") return "text/css; charset=utf-8";
    if (extension == ".wasm") return "application/wasm";
    if (extension == ".svg") return "image/svg+xml";
    if (extension == ".png") return "image/png";
    if (extension == ".webp") return "image/webp";
    if (extension == ".woff2") return "font/woff2";
    return "application/octet-stream";
}

[[nodiscard]] std::optional<std::vector<Asset>> load_assets(const std::filesystem::path& root) {
    if (!std::filesystem::is_directory(root) || !std::filesystem::is_regular_file(root / "index.html")) return std::nullopt;
    std::vector<Asset> assets;
    for (const std::filesystem::directory_entry& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_symlink() || !entry.is_regular_file()) continue;
        const auto relative = entry.path().lexically_relative(root);
        if (relative.empty() || relative.is_absolute()) continue;
        std::ifstream stream(entry.path(), std::ios::binary | std::ios::ate);
        const auto size = stream ? stream.tellg() : std::streampos{-1};
        if (size < 0 || static_cast<std::uintmax_t>(size) > kMaximumMessageBytes) return std::nullopt;
        Asset asset{
            .path = "/" + relative.generic_string(),
            .mime = mime_for(entry.path()),
            .bytes = std::string(static_cast<std::size_t>(size), '\0'),
            .immutable = relative.filename() != "index.html",
        };
        stream.seekg(0);
        if (!asset.bytes.empty() && !stream.read(asset.bytes.data(), size)) return std::nullopt;
        assets.push_back(std::move(asset));
    }
    return assets;
}

[[nodiscard]] const Asset* find_asset(const std::vector<Asset>& assets, const std::string_view path) noexcept {
    const std::string_view requested = path == "/" ? "/index.html" : path;
    for (const Asset& asset : assets)
        if (asset.path == requested) return &asset;
    return nullptr;
}

[[nodiscard]] bool query_unreserved(const unsigned char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || (character >= '0' && character <= '9') ||
           character == '-' || character == '.' || character == '_' || character == '~';
}

[[nodiscard]] std::string percent_encode_query_value(const std::string_view value) {
    static constexpr std::string_view kHex{"0123456789ABCDEF"};
    std::string encoded;
    encoded.reserve(value.size() * 3U);
    for (const unsigned char character : value) {
        if (query_unreserved(character)) {
            encoded.push_back(static_cast<char>(character));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(kHex[character >> 4U]);
        encoded.push_back(kHex[character & 0x0FU]);
    }
    return encoded;
}

}  // namespace

struct BrowserServer::Impl final : std::enable_shared_from_this<Impl> {
    struct UrlState final {
        std::string page;
        std::string websocket;
    };

    std::unique_ptr<uWS::App> app;
    Socket* socket = nullptr;
    us_listen_socket_t* listener = nullptr;
    BrowserRecordRing output;
    std::size_t maximum_output_bytes = kMaximumMessageBytes;
    Callbacks callbacks;
    mutable std::mutex lifecycle_mutex;
    uWS::Loop* wake_loop = nullptr;
    std::optional<UrlState> urls;
    detail::BrowserOutputEpoch output_epoch;
    bool accepting_wakes = false;
    bool wake_pending = false;
    bool stop_wake_pending = false;
    std::uint64_t wake_generation = 0U;
    std::atomic_bool stop_requested = false;
    std::atomic_bool close_requested = false;
    std::atomic_bool running = false;
    std::atomic_bool connected = false;
    std::atomic_bool finalization_required = false;
    std::atomic_size_t pending_enqueued = 0U;
    std::atomic_size_t pending_transient_dropped = 0U;
    std::atomic_size_t pending_critical_closed = 0U;
    std::atomic_size_t pending_invalid = 0U;
    bool backpressured = false;
    bool detached = true;
    bool owner_run_active = false;
    bool initialized_once = false;
    unsigned short port = 0U;
    std::uint64_t next_peer_generation = 0U;
    std::uint64_t active_peer_generation = 0U;
    const std::thread::id owner_thread;
    std::string session_token;
    std::string page_query;
    std::string expected_origin;
    std::vector<Asset> assets;

    explicit Impl(const std::thread::id owner) noexcept : owner_thread(owner) {}

    [[nodiscard]] bool on_owner_thread() const noexcept { return owner_thread == std::this_thread::get_id(); }

    void trace(const BrowserServerEvent event, const std::size_t value = 0U) const noexcept {
        if (callbacks.diagnostic != nullptr) callbacks.diagnostic(callbacks.context.get(), event, value);
    }

    using DeferredWake = void (Impl::*)(std::uint64_t);

    [[nodiscard]] bool request_deferred_wake_locked(bool& pending, const DeferredWake handler) noexcept {
        if (!accepting_wakes || wake_loop == nullptr) return false;
        if (pending) return true;
        try {
            const std::weak_ptr<Impl> weak = weak_from_this();
            const std::uint64_t generation = wake_generation;
            pending = true;
            wake_loop->defer([weak, generation, handler] {
                if (const auto self = weak.lock()) (self.get()->*handler)(generation);
            });
            return true;
        } catch (...) {
            pending = false;
            return false;
        }
    }

    [[nodiscard]] bool request_wake_locked() noexcept { return request_deferred_wake_locked(wake_pending, &Impl::deferred_wake); }

    [[nodiscard]] bool request_wake() noexcept {
        std::scoped_lock lock(lifecycle_mutex);
        return request_wake_locked();
    }

    [[nodiscard]] bool request_stop_wake_locked() noexcept { return request_deferred_wake_locked(stop_wake_pending, &Impl::deferred_stop); }

    void withdraw_wakes() noexcept {
        std::scoped_lock lock(lifecycle_mutex);
        accepting_wakes = false;
        wake_loop = nullptr;
        wake_pending = false;
        stop_wake_pending = false;
        ++wake_generation;
        urls.reset();
    }

    void begin_output_epoch(const std::uint64_t generation) noexcept {
        std::scoped_lock lock(lifecycle_mutex);
        output_epoch.begin_open(generation);
        output.clear();
    }

    void finish_output_epoch(const std::uint64_t generation) noexcept {
        std::scoped_lock lock(lifecycle_mutex);
        output_epoch.finish_open(generation);
    }

    void close_output_epoch() noexcept {
        std::scoped_lock lock(lifecycle_mutex);
        output_epoch.close();
        output.clear();
    }

    [[nodiscard]] bool active_peer(Socket* const peer) const noexcept {
        if (peer == nullptr || peer != socket) return false;
        const Peer* const state = peer->getUserData();
        return !state->lifecycle.closure_notified && state->lifecycle.generation == active_peer_generation;
    }

    void notify_peer_closed(Socket* const peer) noexcept {
        if (peer == nullptr) return;
        Peer* const state = peer->getUserData();
        if (!state->lifecycle.begin_close()) return;
        if (peer == socket) {
            socket = nullptr;
            active_peer_generation = 0U;
            backpressured = false;
            connected.store(false, std::memory_order_release);
            close_output_epoch();
        }
        trace(BrowserServerEvent::PeerClosed, state->lifecycle.generation);
        callbacks.closed(callbacks.context.get());
    }

    void close_peer_on_owner() noexcept {
        close_requested.store(false, std::memory_order_release);
        Socket* const peer = socket;
        if (peer == nullptr) {
            close_output_epoch();
            return;
        }
        notify_peer_closed(peer);
        peer->close();
    }

    void flush_worker_diagnostics() noexcept {
        if (callbacks.diagnostic == nullptr) return;
        if (const auto count = pending_invalid.exchange(0U, std::memory_order_acq_rel); count != 0U)
            trace(BrowserServerEvent::InvalidMessage, count);
        if (const auto count = pending_transient_dropped.exchange(0U, std::memory_order_acq_rel); count != 0U)
            trace(BrowserServerEvent::TransientDropped, count);
        if (const auto count = pending_critical_closed.exchange(0U, std::memory_order_acq_rel); count != 0U)
            trace(BrowserServerEvent::CriticalCapacityClosed, count);
        if (const auto count = pending_enqueued.exchange(0U, std::memory_order_acq_rel); count != 0U)
            trace(BrowserServerEvent::RecordEnqueued, count);
    }

    void drain() noexcept {
        if (close_requested.exchange(false, std::memory_order_acq_rel)) {
            close_peer_on_owner();
            return;
        }
        if (socket == nullptr || backpressured) return;
        while (socket != nullptr) {
            auto record = output.pop();
            if (!record) return;
            const auto bytes = std::span<const std::byte>(record->bytes);
            const std::string_view message{reinterpret_cast<const char*>(bytes.data()), bytes.size()};
            const Socket::SendStatus status = socket->send(message, uWS::OpCode::BINARY, false);
            if (status == Socket::DROPPED) {
                close_peer_on_owner();
                return;
            }
            if (status == Socket::BACKPRESSURE) {
                backpressured = true;
                trace(BrowserServerEvent::WriteBackpressured, bytes.size());
                return;
            }
            trace(BrowserServerEvent::WriteAccepted, bytes.size());
        }
    }

    void stop_on_owner() noexcept {
        if (detached) return;
        detached = true;
        stop_requested.store(false, std::memory_order_release);
        withdraw_wakes();
        if (listener != nullptr) us_listen_socket_close(0, listener);
        listener = nullptr;
        running.store(false, std::memory_order_release);
        if (socket != nullptr)
            close_peer_on_owner();
        else
            close_output_epoch();
    }

    void finalize_on_owner() noexcept {
        stop_on_owner();
        app.reset();
        callbacks = {};
        assets.clear();
        session_token.clear();
        page_query.clear();
        expected_origin.clear();
        port = 0U;
        finalization_required.store(false, std::memory_order_release);
    }

    void deferred_wake(const std::uint64_t generation) noexcept {
        {
            std::scoped_lock lock(lifecycle_mutex);
            if (!accepting_wakes || wake_loop == nullptr || generation != wake_generation) return;
            wake_pending = false;
        }
        flush_worker_diagnostics();
        if (stop_requested.load(std::memory_order_acquire)) {
            stop_on_owner();
            return;
        }
        drain();
    }

    void deferred_stop(const std::uint64_t generation) noexcept {
        {
            std::scoped_lock lock(lifecycle_mutex);
            if (!accepting_wakes || wake_loop == nullptr || generation != wake_generation) return;
            stop_wake_pending = false;
        }
        flush_worker_diagnostics();
        stop_on_owner();
    }

    [[nodiscard]] BrowserRecordPush publish(BrowserOutputRecord record) noexcept {
        const bool critical = record.priority == BrowserRecordPriority::Critical;
        const bool owner = on_owner_thread();
        std::scoped_lock lock(lifecycle_mutex);
        if (!accepting_wakes || wake_loop == nullptr || !output_epoch.admits(owner) || stop_requested.load(std::memory_order_acquire) ||
            close_requested.load(std::memory_order_acquire))
            return critical ? BrowserRecordPush::ClosePeer : BrowserRecordPush::Dropped;
        const std::size_t bytes = record.bytes.size();
        if (bytes == 0U || bytes > maximum_output_bytes) {
            if (callbacks.diagnostic != nullptr) pending_invalid.fetch_add(1U, std::memory_order_relaxed);
            if (critical) {
                if (callbacks.diagnostic != nullptr) pending_critical_closed.fetch_add(1U, std::memory_order_relaxed);
                close_requested.store(true, std::memory_order_release);
                static_cast<void>(request_wake_locked());
                return BrowserRecordPush::ClosePeer;
            }
            if (callbacks.diagnostic != nullptr) {
                pending_transient_dropped.fetch_add(1U, std::memory_order_relaxed);
                static_cast<void>(request_wake_locked());
            }
            return BrowserRecordPush::Dropped;
        }
        if (!request_wake_locked()) return critical ? BrowserRecordPush::ClosePeer : BrowserRecordPush::Dropped;
        const BrowserRecordPush result = output.push(std::move(record));
        switch (result) {
            case BrowserRecordPush::Enqueued:
                if (callbacks.diagnostic != nullptr) pending_enqueued.fetch_add(1U, std::memory_order_relaxed);
                break;
            case BrowserRecordPush::Dropped:
                if (callbacks.diagnostic != nullptr) pending_transient_dropped.fetch_add(1U, std::memory_order_relaxed);
                break;
            case BrowserRecordPush::ClosePeer:
                if (callbacks.diagnostic != nullptr) pending_critical_closed.fetch_add(1U, std::memory_order_relaxed);
                close_requested.store(true, std::memory_order_release);
                break;
        }
        return result;
    }

    void request_stop() noexcept {
        stop_requested.store(true, std::memory_order_release);
        std::scoped_lock lock(lifecycle_mutex);
        static_cast<void>(request_stop_wake_locked());
    }

    void request_peer_close() noexcept {
        close_requested.store(true, std::memory_order_release);
        static_cast<void>(request_wake());
    }

    [[nodiscard]] std::optional<std::string> page_url() const {
        std::scoped_lock lock(lifecycle_mutex);
        if (!urls) return std::nullopt;
        return urls->page;
    }

    [[nodiscard]] std::optional<std::string> websocket_url() const {
        std::scoped_lock lock(lifecycle_mutex);
        if (!urls) return std::nullopt;
        return urls->websocket;
    }
};

BrowserServer::BrowserServer() : impl_(std::make_shared<Impl>(std::this_thread::get_id())) {}

BrowserServer::~BrowserServer() {
    if (impl_->finalization_required.load(std::memory_order_acquire) && !impl_->on_owner_thread()) std::terminate();
    if (!close()) std::terminate();
}

bool BrowserServer::start(Config config, Callbacks callbacks) noexcept {
    const auto& state = impl_;
    try {
        if (!state->on_owner_thread() || state->initialized_once || state->running.load(std::memory_order_acquire) ||
            state->app != nullptr || !callbacks.valid() || config.session_token.empty() || config.maximum_output_bytes == 0U ||
            config.maximum_output_bytes > std::numeric_limits<unsigned int>::max())
            return false;
        auto assets = load_assets(config.asset_root);
        if (!assets) return false;
        state->callbacks = std::move(callbacks);
        state->maximum_output_bytes = config.maximum_output_bytes;
        state->session_token = std::move(config.session_token);
        state->page_query = std::move(config.page_query);
        state->assets = std::move(*assets);
        state->app = std::make_unique<uWS::App>();
        state->initialized_once = true;
        state->finalization_required.store(true, std::memory_order_release);
        state->detached = false;
        const std::weak_ptr<Impl> weak = state;
        state->app->ws<Peer>(
            "/mmltk",
            {.compression = uWS::DISABLED,
             .maxPayloadLength = static_cast<unsigned int>(kMaximumMessageBytes),
             .idleTimeout = 64,
             .maxBackpressure = static_cast<unsigned int>(state->maximum_output_bytes),
             .closeOnBackpressureLimit = false,
             .upgrade =
                 [weak](uWS::HttpResponse<false>* response, uWS::HttpRequest* request, us_socket_context_t* context) noexcept {
                     const auto owner = weak.lock();
                     if (!owner) {
                         response->writeStatus("503 Service Unavailable")->end("unavailable");
                         return;
                     }
                     const bool valid_session =
                         mmltk::common::types::constant_time_equal(request->getQuery("session"), owner->session_token);
                     const bool valid_origin = request->getHeader("origin") == owner->expected_origin;
                     if (!valid_session || !valid_origin) {
                         response->writeStatus("403 Forbidden")->end("forbidden");
                         return;
                     }
                     response->upgrade<Peer>({}, request->getHeader("sec-websocket-key"), request->getHeader("sec-websocket-protocol"),
                                             request->getHeader("sec-websocket-extensions"), context);
                 },
             .open =
                 [weak](Socket* peer) noexcept {
                     const auto owner = weak.lock();
                     if (!owner) {
                         peer->close();
                         return;
                     }
                     if (owner->socket != nullptr) {
                         owner->trace(BrowserServerEvent::PeerReplaced);
                         owner->close_peer_on_owner();
                     }
                     if (owner->next_peer_generation == std::numeric_limits<std::uint64_t>::max()) {
                         peer->close();
                         return;
                     }
                     const std::uint64_t generation = ++owner->next_peer_generation;
                     owner->begin_output_epoch(generation);
                     peer->getUserData()->lifecycle.opened(generation);
                     owner->socket = peer;
                     owner->active_peer_generation = generation;
                     owner->backpressured = false;
                     owner->connected.store(true, std::memory_order_release);
                     owner->callbacks.opened(owner->callbacks.context.get());
                     owner->finish_output_epoch(generation);
                     owner->trace(BrowserServerEvent::PeerOpened, generation);
                     owner->drain();
                 },
             .message =
                 [weak](Socket* peer, const std::string_view message, const uWS::OpCode opcode) noexcept {
                     const auto owner = weak.lock();
                     if (!owner) {
                         peer->close();
                         return;
                     }
                     if (!owner->active_peer(peer) || opcode != uWS::OpCode::BINARY || message.empty() ||
                         message.size() > kMaximumMessageBytes) {
                         owner->trace(BrowserServerEvent::InvalidMessage, message.size());
                         if (owner->active_peer(peer))
                             owner->close_peer_on_owner();
                         else
                             peer->close();
                         return;
                     }
                     const auto bytes = std::span<const std::byte>{reinterpret_cast<const std::byte*>(message.data()), message.size()};
                     owner->trace(BrowserServerEvent::BinaryReceived, message.size());
                     if (!owner->callbacks.record(owner->callbacks.context.get(), bytes)) owner->close_peer_on_owner();
                 },
             .drain =
                 [weak](Socket* peer) noexcept {
                     const auto owner = weak.lock();
                     if (owner && owner->active_peer(peer)) {
                         owner->backpressured = false;
                         owner->trace(BrowserServerEvent::WriteDrained);
                         owner->drain();
                     }
                 },
             .close =
                 [weak](Socket* peer, int, std::string_view) noexcept {
                     if (const auto owner = weak.lock()) owner->notify_peer_closed(peer);
                 }});
        state->app->get("/health", [](auto* response, auto*) { response->writeHeader("Content-Type", "text/plain")->end("ok"); });
        state->app->get("/*", [weak](uWS::HttpResponse<false>* response, uWS::HttpRequest* request) {
            const auto owner = weak.lock();
            if (!owner) {
                response->writeStatus("503 Service Unavailable")->end("unavailable");
                return;
            }
            const Asset* asset = find_asset(owner->assets, request->getUrl());
            if (asset == nullptr) {
                response->writeStatus("404 Not Found")->end("not found");
                return;
            }
            response->writeHeader("Content-Type", asset->mime)
                ->writeHeader("Cache-Control", asset->immutable ? "public, max-age=31536000, immutable" : "no-store")
                ->end(asset->bytes);
        });
        state->app->listen("127.0.0.1", 0, [weak](us_listen_socket_t* listener) noexcept {
            const auto owner = weak.lock();
            if (!owner) return;
            owner->listener = listener;
            const int port = listener == nullptr ? 0 : us_socket_local_port(0, reinterpret_cast<us_socket_t*>(listener));
            owner->port = port > 0 && port <= std::numeric_limits<unsigned short>::max() ? static_cast<unsigned short>(port) : 0U;
        });
        if (state->listener == nullptr || state->port == 0U) {
            state->finalize_on_owner();
            return false;
        }
        state->expected_origin = "http://127.0.0.1:" + std::to_string(state->port);
        const std::string websocket = "ws://127.0.0.1:" + std::to_string(state->port) + "/mmltk?session=" + state->session_token;
        std::string page =
            state->expected_origin + "/?session=" + state->session_token + "&mmltk_ws_url=" + percent_encode_query_value(websocket);
        if (!state->page_query.empty()) page += "&" + state->page_query;
        state->stop_requested.store(false, std::memory_order_release);
        state->close_requested.store(false, std::memory_order_release);
        {
            std::scoped_lock lock(state->lifecycle_mutex);
            ++state->wake_generation;
            state->wake_loop = state->app->getLoop();
            state->output_epoch.close();
            state->output.clear();
            state->urls = Impl::UrlState{.page = std::move(page), .websocket = websocket};
            state->accepting_wakes = true;
        }
        state->running.store(true, std::memory_order_release);
        state->trace(BrowserServerEvent::Started, state->port);
        return true;
    } catch (...) {
        if (state->on_owner_thread()) state->finalize_on_owner();
        return false;
    }
}

void BrowserServer::run() {
    const auto& state = impl_;
    if (!state->on_owner_thread() || !state->app) return;
    if (state->stop_requested.load(std::memory_order_acquire)) state->stop_on_owner();
    state->owner_run_active = true;
    try {
        state->app->run();
    } catch (...) {
        state->owner_run_active = false;
        state->finalize_on_owner();
        throw;
    }
    state->owner_run_active = false;
    state->finalize_on_owner();
}

void BrowserServer::stop() noexcept { impl_->request_stop(); }

bool BrowserServer::close() noexcept {
    const auto& state = impl_;
    if (!state->on_owner_thread()) {
        if (!state->finalization_required.load(std::memory_order_acquire)) return true;
        state->request_stop();
        return false;
    }
    if (state->owner_run_active) {
        state->stop_on_owner();
        return false;
    }
    state->finalize_on_owner();
    return true;
}

void BrowserServer::close_peer() noexcept { impl_->request_peer_close(); }

BrowserRecordPush BrowserServer::publish(BrowserOutputRecord record) noexcept { return impl_->publish(std::move(record)); }

bool BrowserServer::running() const noexcept { return impl_->running.load(std::memory_order_acquire); }

bool BrowserServer::connected() const noexcept { return impl_->connected.load(std::memory_order_acquire); }

std::size_t BrowserServer::queued_records() const noexcept { return impl_->output.size(); }

std::optional<std::string> BrowserServer::page_url() const { return impl_->page_url(); }

std::optional<std::string> BrowserServer::websocket_url() const { return impl_->websocket_url(); }

}  // namespace mmltk::frameworks::transport
