#include "src/controller/browser/application_browser_host.h"
#include "src/frameworks/gpu/system_image_runtime.h"

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <meta>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "src/common/io/scoped_fd.h"
#include "src/controller/browser/application_materializer.h"
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/controller/services/file_dialog_system.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
#include "filesystem_test_utils.hpp"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/subsystems/system/compute_systems.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/train/training_system.h"
#include "src/controller/subsystems/upscale/upscale_system.h"
#include "src/frameworks/gpu/tests/fake_image_backend.h"

namespace mmltk::controller::browser {
namespace {

namespace transport = mmltk::frameworks::transport;

template <class Composition, class SystemCell>
[[nodiscard]] consteval bool append_reflected_event_ids(std::vector<std::uint64_t>& identities) {
    bool matched = true;
    using Events = typename SystemCell::type::event_type;
    application_schema_detail::Variant<Events>::Visit([&]<class Event>() {
        using Identity = ApplicationEventIdentity<Composition, SystemCell::pointer, Event>;
        matched = matched && Identity::system_id == SystemCell::stable_id && Identity::event_id != 0U;
        identities.push_back(Identity::event_id);
    });
    return matched;
}

template <class Composition>
[[nodiscard]] consteval bool reflected_application_ids_match_members() {
    std::size_t count = 0U;
    bool matched = true;
    template for (constexpr auto reflected_member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^Composition, std::meta::access_context::unchecked()))) {
        using Reflected = ReflectedSystem<Composition, reflected_member>;
        matched = matched && application_system_stable_id<Composition, Reflected::pointer>() == Reflected::stable_id;
        ++count;
    }
    return matched && count == 13U;
}

template <class Composition>
[[nodiscard]] consteval bool reflected_event_ids_are_exhaustive() {
    std::vector<std::uint64_t> identities;
    std::size_t system_count = 0U;
    bool matched = true;
    template for (constexpr auto reflected_member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^Composition, std::meta::access_context::unchecked()))) {
        using SystemCell = ReflectedSystem<Composition, reflected_member>;
        matched = matched && append_reflected_event_ids<Composition, SystemCell>(identities);
        ++system_count;
    }
    for (std::size_t left = 0U; left < identities.size(); ++left)
        for (std::size_t right = left + 1U; right < identities.size(); ++right)
            matched = matched && identities[left] != identities[right];
    return matched && system_count == 13U && identities.size() >= system_count;
}

TEST_CASE("host event identities derive from every application member") {
    STATIC_REQUIRE(reflected_application_ids_match_members<ApplicationSystems>());
    STATIC_REQUIRE(reflected_event_ids_are_exhaustive<ApplicationSystems>());
}

enum class OpenPressure : std::uint8_t {
    None,
    Transient,
    Critical,
    LatestState,
};

struct HostCallbackContext final {
    transport::BrowserServer::Callbacks host;
    ApplicationBrowserHost* owner = nullptr;
    OpenPressure pressure = OpenPressure::None;
    std::mutex mutex;
    std::condition_variable changed;
    std::array<std::size_t, 12U> diagnostics{};

    static void Opened(void* opaque) noexcept {
        auto& self = *static_cast<HostCallbackContext*>(opaque);
        self.host.opened(self.host.context.get());
        if (self.pressure == OpenPressure::None) return;
        const auto delivery = self.pressure == OpenPressure::LatestState ? contracts::reflection::EventDelivery::LatestState
                              : self.pressure == OpenPressure::Critical  ? contracts::reflection::EventDelivery::Critical
                                                                         : contracts::reflection::EventDelivery::Transient;
        for (std::size_t index = 0U; index < transport::kBrowserRecordRingCapacity; ++index) {
            self.owner->publish(SystemEvent{
                .system_id = 1U,
                .event_id = self.pressure == OpenPressure::LatestState ? 1U : index + 1U,
                .delivery = delivery,
                .state_revision = index + 1U,
                .value = wire::Value(wire::Value::Object{{"revision", wire::Value(static_cast<std::uint64_t>(index + 1U))}}),
            });
        }
    }

    static bool Record(void* opaque, const std::span<const std::byte> bytes) noexcept {
        auto& self = *static_cast<HostCallbackContext*>(opaque);
        return self.host.record(self.host.context.get(), bytes);
    }

    static void Closed(void* opaque) noexcept {
        auto& self = *static_cast<HostCallbackContext*>(opaque);
        self.host.closed(self.host.context.get());
    }

    static void Diagnostic(void* opaque, const transport::BrowserServerEvent event, const std::size_t value) noexcept {
        auto& self = *static_cast<HostCallbackContext*>(opaque);
        {
            std::scoped_lock lock(self.mutex);
            self.diagnostics[static_cast<std::size_t>(event)] += value == 0U ? 1U : value;
        }
        self.changed.notify_all();
        if (self.host.diagnostic != nullptr) self.host.diagnostic(self.host.context.get(), event, value);
    }

    [[nodiscard]] bool await(const transport::BrowserServerEvent event) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds{2}, [&] { return diagnostics[static_cast<std::size_t>(event)] != 0U; });
    }
};

class RunningHost final {
   public:
    explicit RunningHost(const OpenPressure pressure)
        : owner_([this, pressure] {
              transport::BrowserServer server;
              ApplicationBrowserHost host{server};
              SettingsSystem settings;
              ApplicationSystems systems{.settings = &settings};
              auto context = std::make_shared<HostCallbackContext>();
              context->host = host.callbacks();
              context->owner = &host;
              context->pressure = pressure;
              const transport::BrowserServer::Callbacks callbacks{
                  .context = context,
                  .opened = &HostCallbackContext::Opened,
                  .record = &HostCallbackContext::Record,
                  .closed = &HostCallbackContext::Closed,
                  .diagnostic = &HostCallbackContext::Diagnostic,
              };
              const bool installed = host.install(systems);
              const bool started = installed && server.start({.asset_root = assets_.path(), .session_token = "test-capability"}, callbacks);
              {
                  std::scoped_lock lock(mutex_);
                  server_ = &server;
                  context_ = context;
                  websocket_ = started ? server.websocket_url().value_or("") : std::string{};
                  ready_ = true;
              }
              changed_.notify_all();
              if (started) server.run();
              {
                  std::scoped_lock lock(mutex_);
                  server_ = nullptr;
              }
          }) {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [&] { return ready_; });
    }

    ~RunningHost() {
        {
            std::scoped_lock lock(mutex_);
            if (server_ != nullptr) server_->stop();
        }
        owner_.join();
    }

    [[nodiscard]] const std::string& websocket() const noexcept { return websocket_; }
    [[nodiscard]] std::shared_ptr<HostCallbackContext> context() const {
        std::scoped_lock lock(mutex_);
        return context_;
    }

   private:
    mmltk::testsupport::BrowserAssetDirectory assets_{"mmltk-direct-browser"};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    transport::BrowserServer* server_ = nullptr;
    std::shared_ptr<HostCallbackContext> context_;
    std::string websocket_;
    bool ready_ = false;
    std::thread owner_;
};

class HostAnnotationAlgorithm final : public AnnotationAlgorithm {
   public:
    explicit HostAnnotationAlgorithm(std::shared_ptr<std::atomic_size_t> closed) : closed_(std::move(closed)) {}
    // CLEANUP-IGNORE: This browser-host test double owns an independent annotation fixture and observable behavior.
    AnnotationOperationResult Open(const mmltk::frameworks::gpu::ImagePlaneView source, contracts::AnnotationSceneContent,
                                   VisualRegion) override {
        // CLEANUP-IGNORE: The fixture values deliberately satisfy the production AnnotationUiState validity contract.
        state_.scene.document = contracts::WorkspaceResource::From("direct://browser-host", 1U);
        state_.scene.categories = {{.value = "object"}};
        state_.scene.frame_width = static_cast<std::uint16_t>(source.descriptor.width);
        state_.scene.frame_height = static_cast<std::uint16_t>(source.descriptor.height);
        state_.scene.frame_ready = true;
        state_.document_revision = 1U;
        state_.scene_revision = 1U;
        state_.interaction_revision = 1U;
        state_.scene.document.revision = 1U;
        return {.ui = state_, .detail = {}, .outcome = AnnotationOperationOutcome::Applied};
    }
    AnnotationOperationResult Pointer(const AnnotationPointer&) override {
        return {.ui = state_, .detail = {}, .outcome = AnnotationOperationOutcome::Applied};
    }
    void PeerClosed() noexcept override { closed_->fetch_add(1U, std::memory_order_release); }
    AnnotationOperationResult Edit(const AnnotationEdit&) override {
        ++state_.document_revision;
        ++state_.scene_revision;
        state_.scene.document.revision = state_.document_revision;
        return {.ui = state_, .detail = {}, .outcome = AnnotationOperationOutcome::Applied};
    }
    AnnotationOperationResult Save(std::string_view) override {
        // CLEANUP-IGNORE: This browser-host double returns its own deterministic applied Save result.
        return {.ui = state_, .detail = {}, .outcome = AnnotationOperationOutcome::Applied};
    }
    void Render(const mmltk::frameworks::gpu::ImagePlaneView source, const mmltk::frameworks::gpu::ImagePlaneView clean,
                const mmltk::frameworks::gpu::ImagePlaneView semantic, std::uintptr_t) const override {
        mmltk::frameworks::gpu::test_support::CopyImagePlane(clean, source);
        std::memset(reinterpret_cast<void*>(semantic.data), 0, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
    }

   private:
    std::shared_ptr<std::atomic_size_t> closed_;
    contracts::AnnotationUiState state_;
};

class ReadyHostAnnotation final {
   public:
    ReadyHostAnnotation()
        : backend_(std::make_shared<mmltk::frameworks::gpu::test_support::FakeImageBackend>()),
          source_(std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(
              mmltk::frameworks::gpu::SystemImageRuntimeConfig{.device = 0, .backend = backend_})),
          annotation_(
              {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
              [backend = backend_, closed = closed_](auto revisions) {
                  return std::make_unique<mmltk::frameworks::gpu::SystemImageRuntime>(mmltk::frameworks::gpu::SystemImageRuntimeConfig{
                      .device = 0,
                      .backend = backend,
                      .model = std::make_unique<HostAnnotationAlgorithm>(closed),
                      .input_layout = mmltk::frameworks::gpu::ImageProductLayout::Clean,
                      .output_layout = mmltk::frameworks::gpu::ImageProductLayout::CleanAndSemantic,
                      .product_revisions = std::move(revisions),
                  });
              },
              [this](const VisualFrame& frame) {
                  if (frame.source != identity_) return VisualDocumentRead{};
                  auto document = std::make_shared<VisualDocument>();
                  document->scene.document = contracts::WorkspaceResource::From("test://image", 1U);
                  document->scene.categories.push_back({.value = "object"});
                  return VisualDocumentRead{borrow_matching_visual_product(frame, source_->Borrow()), std::move(document)};
              },
              [this](AnnotationSystem::event_type event) {
                  {
                      std::scoped_lock lock(mutex_);
                      if (const auto* failed = std::get_if<AnnotationFailed>(&event)) failure_ = failed->detail;
                      ++events_;
                  }
                  changed_.notify_all();
              }) {
        source_->Publish(16U, 16U, [](auto, auto, auto) {});
        static_cast<void>(annotation_.Open({.source = visual_frame(identity_, {16U, 16U}, source_->OutputFacts().revision)}));
        const bool ready = Wait([this] { return annotation_.snapshot().ready || !failure_.empty(); });
        INFO("Annotation startup failure: " << failure_);
        REQUIRE(ready);
        REQUIRE(failure_.empty());
    }

    [[nodiscard]] AnnotationSystem& system() noexcept { return annotation_; }
    [[nodiscard]] std::size_t closed() const noexcept { return closed_->load(std::memory_order_acquire); }
    void Flush() {
        const auto before = annotation_.snapshot();
        INFO("Annotation ready: " << before.ready << ", frame valid: " << before.frame.valid() << ", UI valid: " << before.ui.valid());
        REQUIRE(before.ready);
        REQUIRE(before.frame.valid());
        REQUIRE(before.ui.valid());
        const auto admitted = annotation_.Edit({.edit = {.value = AnnotationUndoEdit{}}});
        REQUIRE(Wait([this, revision = admitted.revision] {
            const auto state = annotation_.snapshot();
            return !state.busy && state.revision > revision;
        }));
    }

   private:
    template <class Predicate>
    [[nodiscard]] bool Wait(Predicate predicate) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, std::chrono::seconds{2}, std::move(predicate));
    }

    std::shared_ptr<mmltk::frameworks::gpu::test_support::FakeImageBackend> backend_;
    std::unique_ptr<mmltk::frameworks::gpu::SystemImageRuntime> source_;
    std::shared_ptr<std::atomic_size_t> closed_ = std::make_shared<std::atomic_size_t>(0U);
    PresentationSourceIdentity identity_{PresentationSourceKind::Explore, 1U};
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t events_ = 0U;
    std::string failure_;
    AnnotationSystem annotation_;
};

struct WebSocketFrame final {
    std::uint8_t opcode = 0U;
    std::vector<std::byte> payload;
};

enum class HandshakePolicy : std::uint8_t {
    RequireUpgrade,
    AllowPeerClose,
};

class LoopbackWebSocket final {
   public:
    explicit LoopbackWebSocket(const std::string& url, const HandshakePolicy handshake = HandshakePolicy::RequireUpgrade) {
        const auto authority = url.find("://") + 3U;
        const auto slash = url.find('/', authority);
        const auto colon = url.find(':', authority);
        REQUIRE(authority != std::string::npos);
        REQUIRE(slash != std::string::npos);
        REQUIRE(colon != std::string::npos);
        const auto port = static_cast<std::uint16_t>(std::stoul(url.substr(colon + 1U, slash - colon - 1U)));
        descriptor_ = mmltk::common::io::ScopedFd{::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)};
        REQUIRE(descriptor_.get() >= 0);
        timeval timeout{.tv_sec = 2, .tv_usec = 0};
        REQUIRE(::setsockopt(descriptor_.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::connect(descriptor_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        const std::string origin = "http://127.0.0.1:" + std::to_string(port);
        const std::string request = "GET " + url.substr(slash) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                                    "\r\nOrigin: " + origin +
                                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                    "Sec-WebSocket-Version: 13\r\n\r\n";
        REQUIRE(::send(descriptor_.get(), request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()));
        std::string response;
        while (response.find("\r\n\r\n") == std::string::npos) {
            char byte = '\0';
            const ssize_t received = ::recv(descriptor_.get(), &byte, sizeof(byte), 0);
            if (received == 0) {
                REQUIRE(handshake == HandshakePolicy::AllowPeerClose);
                return;
            }
            REQUIRE(received == static_cast<ssize_t>(sizeof(byte)));
            response.push_back(byte);
        }
        REQUIRE(response.starts_with("HTTP/1.1 101"));
    }

    [[nodiscard]] std::optional<WebSocketFrame> receive() {
        std::array<std::byte, 2U> header{};
        if (!read_exact(header)) return std::nullopt;
        WebSocketFrame frame{
            .opcode = static_cast<std::uint8_t>(std::to_integer<unsigned int>(header[0]) & 0x0fU),
            .payload = {},
        };
        std::uint64_t size = std::to_integer<unsigned int>(header[1]) & 0x7fU;
        if (size == 126U) {
            std::array<std::byte, 2U> extended{};
            REQUIRE(read_exact(extended));
            size = (std::to_integer<std::uint64_t>(extended[0]) << 8U) | std::to_integer<std::uint64_t>(extended[1]);
        } else if (size == 127U) {
            std::array<std::byte, 8U> extended{};
            REQUIRE(read_exact(extended));
            size = 0U;
            for (const auto byte : extended)
                size = (size << 8U) | std::to_integer<std::uint64_t>(byte);
        }
        REQUIRE(size <= kMaxRecordWireBytes);
        frame.payload.resize(static_cast<std::size_t>(size));
        REQUIRE(read_exact(frame.payload));
        return frame;
    }

    void send_binary(const std::span<const std::byte> payload) {
        std::vector<std::byte> frame;
        frame.push_back(std::byte{0x82});
        if (payload.size() < 126U) {
            frame.push_back(std::byte{static_cast<unsigned char>(0x80U | payload.size())});
        } else {
            REQUIRE(payload.size() <= 65535U);
            frame.push_back(std::byte{0xfe});
            frame.push_back(std::byte{static_cast<unsigned char>(payload.size() >> 8U)});
            frame.push_back(std::byte{static_cast<unsigned char>(payload.size())});
        }
        constexpr std::array mask{std::byte{0x31}, std::byte{0x41}, std::byte{0x59}, std::byte{0x26}};
        frame.insert(frame.end(), mask.begin(), mask.end());
        for (std::size_t index = 0U; index < payload.size(); ++index)
            frame.push_back(payload[index] ^ mask[index % mask.size()]);
        REQUIRE(::send(descriptor_.get(), frame.data(), frame.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(frame.size()));
    }

   private:
    template <std::ranges::contiguous_range Range>
    [[nodiscard]] bool read_exact(Range&& destination) {
        auto bytes = std::as_writable_bytes(std::span{destination});
        std::size_t offset = 0U;
        while (offset != bytes.size()) {
            const auto count = ::recv(descriptor_.get(), bytes.data() + offset, bytes.size() - offset, 0);
            if (count <= 0) return false;
            offset += static_cast<std::size_t>(count);
        }
        return true;
    }

    mmltk::common::io::ScopedFd descriptor_;
};

[[nodiscard]] ServerRecord decode(const WebSocketFrame& frame) {
    REQUIRE(frame.opcode == 2U);
    const auto bytes = std::span<const std::byte>{frame.payload};
    auto decoded = decode_server_record({.first = bytes});
    REQUIRE(decoded);
    return std::move(*decoded);
}

[[nodiscard]] std::uint64_t settings_reset_endpoint() {
    std::uint64_t result = 0U;
    ApplicationIntentSurface<ApplicationSystems>::Visit([&]<class Endpoint>() {
        if constexpr (std::same_as<typename Endpoint::signature::owner_type, SettingsSystem> && Endpoint::name == "Reset")
            result = Endpoint::stable_id;
    });
    return result;
}

[[nodiscard]] Interaction explore_viewport_interaction() {
    const auto value = mmltk::frameworks::serialization::reflected_value(ExploreViewportUpdate{});
    REQUIRE(value);
    return {
        .endpoint_id = application_stable_id("explore", "UpdateViewport"),
        .value = *value,
    };
}

TEST_CASE("direct host emits Bootstrap and dispatches intent on a real peer") {
    RunningHost server{OpenPressure::None};
    REQUIRE_FALSE(server.websocket().empty());
    LoopbackWebSocket peer{server.websocket()};
    auto first = peer.receive();
    REQUIRE(first);
    CHECK(std::holds_alternative<Bootstrap>(decode(*first)));

    const auto endpoint = settings_reset_endpoint();
    REQUIRE(endpoint != 0U);
    wire::ByteBuffer intent;
    REQUIRE(encode_client_record(ClientRecord{Intent{
                                     .correlation = 7U,
                                     .endpoint_id = endpoint,
                                     .fields = {},
                                 }},
                                 intent));
    peer.send_binary(intent);
    auto reply_frame = peer.receive();
    REQUIRE(reply_frame);
    const auto reply = decode(*reply_frame);
    REQUIRE(std::holds_alternative<IntentReply>(reply));
    CHECK(std::get<IntentReply>(reply).correlation == 7U);
    CHECK(std::get<IntentReply>(reply).error.has_value());
}

TEST_CASE("direct host closes a real peer on malformed Protocol-13 input") {
    RunningHost server{OpenPressure::None};
    LoopbackWebSocket peer{server.websocket()};
    REQUIRE(peer.receive());
    const std::array malformed{std::byte{0xff}};
    peer.send_binary(malformed);
    const auto terminal = peer.receive();
    CHECK((!terminal || terminal->opcode == 8U));
}

TEST_CASE("direct host keeps a real peer after decoded application interaction rejection") {
    RunningHost server{OpenPressure::None};
    LoopbackWebSocket peer{server.websocket()};
    REQUIRE(peer.receive());

    wire::ByteBuffer interaction;
    REQUIRE(encode_client_record(ClientRecord{explore_viewport_interaction()}, interaction));
    peer.send_binary(interaction);

    wire::ByteBuffer later_intent;
    REQUIRE(encode_client_record(ClientRecord{Intent{
                                     .correlation = 29U,
                                     .endpoint_id = settings_reset_endpoint(),
                                     .fields = {},
                                 }},
                                 later_intent));
    peer.send_binary(later_intent);
    const auto reply_frame = peer.receive();
    REQUIRE(reply_frame);
    const auto reply = decode(*reply_frame);
    REQUIRE(std::holds_alternative<IntentReply>(reply));
    CHECK(std::get<IntentReply>(reply).correlation == 29U);
}

TEST_CASE("application interaction rejection emits bounded endpoint and error diagnostics") {
    int descriptors[2]{-1, -1};
    REQUIRE(::pipe2(descriptors, O_CLOEXEC) == 0);
    mmltk::common::io::ScopedFd reader{descriptors[0]};
    services::DiagnosticsClient diagnostics{mmltk::common::io::ScopedFd{descriptors[1]},
                                            services::DiagnosticsExecutionPolicy::CallerDriven};
    services::RuntimeDiagnostics runtime{diagnostics.producer()};
    transport::BrowserServer server;
    ApplicationBrowserHost host{server, runtime.target()};
    SettingsSystem settings;
    ApplicationSystems systems{.settings = &settings};
    REQUIRE(host.install(systems));

    wire::ByteBuffer interaction;
    REQUIRE(encode_client_record(ClientRecord{explore_viewport_interaction()}, interaction));
    const auto callbacks = host.callbacks();
    CHECK(callbacks.record(callbacks.context.get(), interaction));
    diagnostics.flush();

    // CLEANUP-IGNORE: Interaction diagnostics and benchmark diagnostics own separate pipe records.
    std::array<char, services::DiagnosticsClient::kRecordCapacity> record{};
    const ssize_t size = ::read(reader.get(), record.data(), record.size());
    REQUIRE(size > 0);
    const std::string_view jsonl{record.data(), static_cast<std::size_t>(size)};
    CHECK(jsonl.contains(R"("event":"browser.interaction.rejected")"));
    CHECK(jsonl.contains(R"("participant":"UpdateViewport")"));
    CHECK(jsonl.contains("\"sequence\":" + std::to_string(explore_viewport_interaction().endpoint_id)));
    CHECK(jsonl.contains("\"value\":" + std::to_string(static_cast<std::uint64_t>(contracts::ApplicationErrorCategory::Unavailable))));
    CHECK(jsonl.contains(R"("message":"application system is unavailable")"));
    diagnostics.close(services::DiagnosticsCloseMode::Discard);
}

TEST_CASE("direct host closes a real peer when an interaction endpoint is unknown") {
    RunningHost server{OpenPressure::None};
    LoopbackWebSocket peer{server.websocket()};
    REQUIRE(peer.receive());
    wire::ByteBuffer interaction;
    REQUIRE(encode_client_record(
        ClientRecord{Interaction{.endpoint_id = std::numeric_limits<std::uint64_t>::max(), .value = wire::Value(wire::Value::Object{})}},
        interaction));
    peer.send_binary(interaction);
    const auto terminal = peer.receive();
    CHECK((!terminal || terminal->opcode == 8U));
}

TEST_CASE("browser admission gates Annotation peer-terminal notification") {
    transport::BrowserServer server;
    ApplicationBrowserHost host{server};
    ReadyHostAnnotation ready;
    ApplicationSystems systems{.annotation = &ready.system()};
    REQUIRE(host.install(systems));
    const auto callbacks = host.callbacks();

    callbacks.closed(callbacks.context.get());
    ready.Flush();
    CHECK(ready.closed() == 1U);

    host.close_admission();
    callbacks.closed(callbacks.context.get());
    ready.Flush();
    CHECK(ready.closed() == 1U);
}

TEST_CASE("direct host drops transient pressure in the real open epoch") {
    RunningHost server{OpenPressure::Transient};
    LoopbackWebSocket peer{server.websocket()};
    REQUIRE(peer.receive());
    REQUIRE(server.context()->await(transport::BrowserServerEvent::TransientDropped));
}

TEST_CASE("direct host closes critical pressure in the real open epoch") {
    RunningHost server{OpenPressure::Critical};
    LoopbackWebSocket peer{server.websocket(), HandshakePolicy::AllowPeerClose};
    const auto terminal = peer.receive();
    CHECK((!terminal || terminal->opcode == 8U));
    REQUIRE(server.context()->await(transport::BrowserServerEvent::CriticalCapacityClosed));
}

TEST_CASE("direct host closes the peer when essential state continuity is lost") {
    RunningHost server{OpenPressure::None};
    LoopbackWebSocket peer{server.websocket(), HandshakePolicy::AllowPeerClose};
    REQUIRE(peer.receive());
    server.context()->owner->continuity_lost();
    const auto terminal = peer.receive();
    CHECK((!terminal || terminal->opcode == 8U));
}

TEST_CASE("direct host preserves the final complete state across a snapshot burst") {
    RunningHost server{OpenPressure::LatestState};
    LoopbackWebSocket peer{server.websocket()};
    const auto bootstrap = peer.receive();
    REQUIRE(bootstrap);
    CHECK(std::holds_alternative<Bootstrap>(decode(*bootstrap)));
    const auto frame = peer.receive();
    REQUIRE(frame);
    const auto record = decode(*frame);
    REQUIRE(std::holds_alternative<SystemEvent>(record));
    const auto& event = std::get<SystemEvent>(record);
    CHECK(event.delivery == contracts::reflection::EventDelivery::LatestState);
    CHECK(event.event_id == 1U);
    CHECK(event.value == wire::Value(wire::Value::Object{{"revision", wire::Value(std::uint64_t{64U})}}));
}

}  // namespace
}  // namespace mmltk::controller::browser
