#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "src/frameworks/transport/browser_record_ring.h"

namespace mmltk::frameworks::transport {

enum class BrowserServerEvent : std::uint8_t {
    Started,
    PeerOpened,
    PeerReplaced,
    PeerClosed,
    BinaryReceived,
    InvalidMessage,
    RecordEnqueued,
    TransientDropped,
    CriticalCapacityClosed,
    WriteAccepted,
    WriteBackpressured,
    WriteDrained,
};

// Sole physical local browser server and bounded publication authority.
class BrowserServer final {
   public:
    struct Callbacks final {
        std::shared_ptr<void> context;
        void (*opened)(void*) noexcept = nullptr;
        // Bootstrap has been accepted by the socket and worker output admission is open.
        void (*activated)(void*) noexcept = nullptr;
        bool (*record)(void*, std::span<const std::byte>) noexcept = nullptr;
        void (*closed)(void*) noexcept = nullptr;
        void (*diagnostic)(void*, BrowserServerEvent, std::size_t) noexcept = nullptr;

        [[nodiscard]] bool valid() const noexcept { return context && opened != nullptr && record != nullptr && closed != nullptr; }
    };

    struct Config final {
        std::filesystem::path asset_root;
        std::string session_token;
        std::string page_query{};
        std::size_t maximum_output_bytes = 8U * 1024U * 1024U;
    };

    BrowserServer();
    // If physical initialization occurred, destruction must happen on the
    // construction thread or after that thread completed close() finalization.
    // Destroying an unfinalized server from another thread terminates.
    ~BrowserServer();
    BrowserServer(const BrowserServer&) = delete;
    BrowserServer& operator=(const BrowserServer&) = delete;

    // start() and run() are construction-thread operations. BrowserServer is
    // one-shot: after physical initialization, later start() calls fail.
    [[nodiscard]] bool start(Config config, Callbacks callbacks) noexcept;
    void run();
    // These request operations and the queries below are worker-safe while the
    // BrowserServer facade remains alive.
    void stop() noexcept;
    // Returns true once physical resources are finalized. Off-owner calls
    // request stop and return false while owner finalization remains required.
    [[nodiscard]] bool close() noexcept;
    void close_peer() noexcept;

    // May be called by system workers. Transient records are best effort.
    // Critical capacity failure schedules closure of the current peer.
    [[nodiscard]] BrowserRecordPush publish(BrowserOutputRecord record) noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::size_t queued_records() const noexcept;
    [[nodiscard]] std::optional<std::string> page_url() const;
    [[nodiscard]] std::optional<std::string> websocket_url() const;

   private:
    struct Impl;
    const std::shared_ptr<Impl> impl_;
};

}  // namespace mmltk::frameworks::transport
