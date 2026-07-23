#pragma once

#include "browser/host_api.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::gui {

struct BrowserAppInboundMessage {
    enum class Kind : std::uint8_t {
        Intent = 0,
        CallbacksReady = 1,
        RuntimeCapabilities = 2,
        WorkspaceRelease = 3,
        Rejected = 4,
    };

    Kind kind = Kind::Rejected;
    std::optional<mmltk::browser::IntentMessage> intent;
    mmltk::browser::BrowserRuntimeCapabilityStatus navigator_gpu =
        mmltk::browser::BrowserRuntimeCapabilityStatus::Unknown;
    nlohmann::json capabilities;
    std::string error;
    std::uint64_t workspace_generation = 0U;
    std::uint64_t workspace_revision = 0U;
    std::uint32_t workspace_slot = 0U;
    bool workspace_encoded = false;
    std::uint64_t connection_epoch = 0U;
};

class BrowserAppServer {
   public:
    struct Config {
        std::filesystem::path asset_root;
        std::string session_token;
        std::size_t max_inbound_messages = 256U;
        std::size_t max_inbound_bytes = std::size_t{4U} * 1024U * 1024U;
        std::size_t max_control_messages = 64U;
        std::size_t max_control_bytes = std::size_t{1024U} * 1024U;
        std::size_t max_outbound_message_bytes = std::size_t{8U} * 1024U * 1024U;
        std::size_t max_outbound_bytes = std::size_t{12U} * 1024U * 1024U;
        bool ui_trace_enabled = false;
    };

    struct Callbacks {
        std::function<void()> on_inbound_ready;
        std::function<void()> on_connection_changed;
    };

    BrowserAppServer();
    ~BrowserAppServer();

    BrowserAppServer(const BrowserAppServer&) = delete;
    BrowserAppServer& operator=(const BrowserAppServer&) = delete;

    [[nodiscard]] bool start(Config config, Callbacks callbacks);
    void stop();

    [[nodiscard]] std::optional<std::string> page_url() const;
    [[nodiscard]] std::optional<std::string> websocket_url() const;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] std::uint64_t connection_epoch() const noexcept;

    void drain_inbound(std::vector<BrowserAppInboundMessage>& messages, std::size_t max_messages);
    [[nodiscard]] bool publish_bridge_state(std::string message);
    [[nodiscard]] bool publish_snapshot(std::string message);
    [[nodiscard]] bool publish_workspace_present(std::string message, std::function<void()> on_displaced);
    [[nodiscard]] bool publish_error(const std::string& message);

   private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  
