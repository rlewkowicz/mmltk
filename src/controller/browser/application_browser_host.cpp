#include "src/controller/browser/application_browser_host.h"

#include <atomic>
#include <mutex>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "src/controller/browser/application_materializer.h"
#include "src/controller/contracts/application_systems.h"

namespace mmltk::controller::browser {
namespace {

namespace transport = mmltk::frameworks::transport;
inline constexpr auto kInputProgressWireCapacity =
    mmltk::frameworks::serialization::reflected_maximum_cbor_bytes<std::variant<InputProgress>>();

[[nodiscard]] constexpr transport::BrowserRecordPriority priority(const contracts::reflection::EventDelivery delivery) noexcept {
    return delivery != contracts::reflection::EventDelivery::Transient ? transport::BrowserRecordPriority::Critical
                                                                       : transport::BrowserRecordPriority::Transient;
}

[[nodiscard]] constexpr std::string_view diagnostic_name(const transport::BrowserServerEvent event) noexcept {
    switch (event) {
        case transport::BrowserServerEvent::Started:
            return "browser.server.started";
        case transport::BrowserServerEvent::PeerOpened:
            return "browser.server.peer_opened";
        case transport::BrowserServerEvent::PeerReplaced:
            return "browser.server.peer_replaced";
        case transport::BrowserServerEvent::PeerClosed:
            return "browser.server.peer_closed";
        case transport::BrowserServerEvent::BinaryReceived:
            return "browser.server.binary_received";
        case transport::BrowserServerEvent::InvalidMessage:
            return "browser.server.invalid_message";
        case transport::BrowserServerEvent::RecordEnqueued:
            return "browser.server.record_enqueued";
        case transport::BrowserServerEvent::TransientDropped:
            return "browser.server.transient_dropped";
        case transport::BrowserServerEvent::CriticalCapacityClosed:
            return "browser.server.critical_capacity_closed";
        case transport::BrowserServerEvent::WriteAccepted:
            return "browser.server.write_accepted";
        case transport::BrowserServerEvent::WriteBackpressured:
            return "browser.server.write_backpressured";
        case transport::BrowserServerEvent::WriteDrained:
            return "browser.server.write_drained";
    }
    return {};
}

}  // namespace

struct ApplicationBrowserHost::Impl final {
    Impl(transport::BrowserServer& server_value, const services::RuntimeDiagnosticTarget diagnostics_value)
        : server(&server_value), diagnostics(diagnostics_value) {}

    [[nodiscard]] bool install(ApplicationSystems& value) noexcept {
        ApplicationSystems* expected = nullptr;
        return systems.compare_exchange_strong(expected, &value, std::memory_order_release, std::memory_order_acquire) ||
               expected == &value;
    }

    [[nodiscard]] bool publish_record(const ServerRecord& record, const transport::BrowserRecordPriority record_priority) noexcept {
        try {
            auto encoded =
                record_priority == transport::BrowserRecordPriority::Progress ? server->acquire_progress_storage() : wire::ByteBuffer{};
            encoded.reserve(record_priority == transport::BrowserRecordPriority::Progress ? kInputProgressWireCapacity
                                                                                          : kMaxIntentValueBytes);
            const auto encoding = [&]() -> std::expected<void, RecordCodecError> {
                if (record_priority != transport::BrowserRecordPriority::Progress) return encode_server_record(record, encoded);
                encoded.resize(kInputProgressWireCapacity);
                mmltk::frameworks::serialization::FixedCborEncoder writer(encoded);
                if (!mmltk::frameworks::serialization::encode_fixed(writer, record))
                    return std::unexpected(RecordCodecError{writer.error()});
                encoded.resize(writer.size());
                return {};
            }();
            if (!encoding) {
                diagnostics.Emit([&] {
                    return services::RuntimeDiagnosticFact{
                        .owner = contracts::DiagnosticOwner::BrowserRuntime,
                        .event = "browser.record.encode_rejected",
                        .value = static_cast<std::uint64_t>(encoding.error().code),
                        .detail = static_cast<std::uint64_t>(record_priority),
                    };
                });
                if (record_priority != transport::BrowserRecordPriority::Transient) continuity_lost();
                return false;
            }
            const auto* state = std::get_if<SystemEvent>(&record);
            if (state != nullptr && state->delivery != contracts::reflection::EventDelivery::LatestState) state = nullptr;
            const auto result = server->publish({
                .bytes = std::move(encoded),
                .priority = record_priority,
                .state_system = state != nullptr ? state->system_id : 0U,
                .state_event = state != nullptr ? state->event_id : 0U,
                .state_revision = state != nullptr ? state->state_revision : 0U,
            });
            const bool accepted =
                record_priority == transport::BrowserRecordPriority::Transient || result == transport::BrowserRecordPush::Enqueued;
            if (!accepted) {
                diagnostics.Emit([&] {
                    return services::RuntimeDiagnosticFact{
                        .owner = contracts::DiagnosticOwner::BrowserRuntime,
                        .event = "browser.record.publish_rejected",
                        .value = static_cast<std::uint64_t>(result),
                        .detail = static_cast<std::uint64_t>(record_priority),
                    };
                });
                continuity_lost();
            }
            return accepted;
        } catch (...) {
            if (record_priority != transport::BrowserRecordPriority::Transient) continuity_lost();
            if (diagnostics.valid()) {
                const auto error = map_current_exception();
                diagnostics.Emit([&] {
                    return services::RuntimeDiagnosticFact{
                        .owner = contracts::DiagnosticOwner::BrowserRuntime,
                        .event = "browser.record.publish_exception",
                        .value = static_cast<std::uint64_t>(error.category),
                        .detail = static_cast<std::uint64_t>(record_priority),
                        .message = error.detail,
                    };
                });
            }
            return false;
        }
    }

    void opened() noexcept {
        try {
            auto* installed = systems.load(std::memory_order_acquire);
            if (admission.load(std::memory_order_acquire) && installed != nullptr) {
                if (installed->presentation != nullptr) installed->presentation->SetApplicationPeerConnected(true);
                std::uint64_t epoch;
                {
                    std::scoped_lock lock(input_mutex);
                    if (input_epoch == std::numeric_limits<std::uint64_t>::max())
                        throw contracts::UnavailableError("input epoch exhausted");
                    epoch = ++input_epoch;
                    consumed_sequence = 0U;
                    input_active = true;
                    auto bootstrap = materialize_bootstrap(*installed);
                    bootstrap.input_epoch = epoch;
                    (void)publish_record(bootstrap, transport::BrowserRecordPriority::Critical);
                }
                return;
            }
        } catch (...) {}
        continuity_lost();
    }

    void activated() noexcept {
        auto* installed = systems.load(std::memory_order_acquire);
        if (!admission.load(std::memory_order_acquire) || !installed) return;
        std::uint64_t epoch;
        {
            std::scoped_lock lock(input_mutex);
            epoch = input_epoch;
        }
        try {
            if (installed->annotation)
                installed->annotation->SetInputPeer(epoch, [this](AnnotationInputProgress value) {
                    std::scoped_lock progress_lock(input_mutex);
                    if (value.epoch != input_epoch || !input_active) return;
                    consumed_sequence = value.consumed_sequence;
                    InputProgress progress{.progress = std::move(value)};
                    (void)publish_record(progress, progress.progress.rejection ? transport::BrowserRecordPriority::Critical
                                                                               : transport::BrowserRecordPriority::Progress);
                });
        } catch (...) { continuity_lost(); }
    }

    [[nodiscard]] bool record(const std::span<const std::byte> bytes) noexcept {
        auto* installed = systems.load(std::memory_order_acquire);
        if (!admission.load(std::memory_order_acquire) || installed == nullptr) {
            diagnostics.Emit([&] {
                return services::RuntimeDiagnosticFact{
                    .owner = contracts::DiagnosticOwner::BrowserRuntime,
                    .event = "browser.record.unavailable",
                    .value = bytes.size(),
                    .detail = installed == nullptr ? 1U : 0U,
                };
            });
            return false;
        }
        try {
            if (is_interaction_record(bytes)) {
                auto view = decode_interaction_view(bytes);
                return view && interaction(*installed, *view);
            }
            auto decoded = decode_client_record({.first = bytes});
            if (!decoded) {
                diagnostics.Emit([&] {
                    return services::RuntimeDiagnosticFact{
                        .owner = contracts::DiagnosticOwner::BrowserRuntime,
                        .event = "browser.record.decode_rejected",
                        .value = bytes.size(),
                        .detail = static_cast<std::uint64_t>(decoded.error().code),
                    };
                });
                return false;
            }
            return std::visit(
                [this, installed]<class Record>(Record value) {
                    using Type = std::remove_cvref_t<Record>;
                    if constexpr (std::same_as<Type, Intent>) {
                        const auto endpoint_id = value.endpoint_id;
                        const auto correlation = value.correlation;
                        const auto reply = dispatch_intent(*installed, std::move(value));
                        if (reply.error.has_value()) {
                            diagnostics.Emit([&] {
                                return services::RuntimeDiagnosticFact{
                                    .owner = contracts::DiagnosticOwner::BrowserRuntime,
                                    .event = "browser.intent.rejected",
                                    .sequence = endpoint_id,
                                    .value = correlation,
                                    .detail = static_cast<std::uint64_t>(reply.error->category),
                                    .message = reply.error->detail,
                                };
                            });
                        } else {
                            diagnostics.Emit([&] {
                                return services::RuntimeDiagnosticFact{
                                    .owner = contracts::DiagnosticOwner::BrowserRuntime,
                                    .event = "browser.intent.accepted",
                                    .sequence = endpoint_id,
                                    .value = correlation,
                                };
                            });
                        }
                        const bool published = publish_record(reply, transport::BrowserRecordPriority::Critical);
                        if (!published) {
                            diagnostics.Emit([&] {
                                return services::RuntimeDiagnosticFact{
                                    .owner = contracts::DiagnosticOwner::BrowserRuntime,
                                    .event = "browser.intent.reply_rejected",
                                    .sequence = endpoint_id,
                                    .value = correlation,
                                };
                            });
                        }
                        return published;
                    } else if constexpr (std::same_as<Type, Interaction>) {
                        return interaction(*installed, InteractionView{value});
                    } else {
                        auto* presentation = installed->presentation;
                        if (presentation == nullptr) {
                            diagnostics.Emit([&] {
                                return services::RuntimeDiagnosticFact{
                                    .owner = contracts::DiagnosticOwner::BrowserRuntime,
                                    .event = "browser.renderer_observation.unavailable",
                                    .sequence = value.sample_revision,
                                    .value = static_cast<std::uint64_t>(value.kind),
                                };
                            });
                            return false;
                        }
                        presentation->Observe({
                            .completed_sample = value.kind == RendererObservationKind::Presented ? value.sample_revision : 0U,
                            .redraw_requested = value.kind != RendererObservationKind::Presented,
                        });
                        diagnostics.Emit([&] {
                            return services::RuntimeDiagnosticFact{
                                .owner = contracts::DiagnosticOwner::BrowserRuntime,
                                .event = "browser.renderer_observation.accepted",
                                .sequence = value.sample_revision,
                                .value = static_cast<std::uint64_t>(value.kind),
                                .context = {.capacity_width = value.width, .capacity_height = value.height},
                            };
                        });
                        return true;
                    }
                },
                std::move(*decoded));
        } catch (...) {
            if (diagnostics.valid()) {
                const auto error = map_current_exception();
                diagnostics.Emit([&] {
                    return services::RuntimeDiagnosticFact{
                        .owner = contracts::DiagnosticOwner::BrowserRuntime,
                        .event = "browser.record.exception",
                        .value = static_cast<std::uint64_t>(error.category),
                        .message = error.detail,
                    };
                });
            }
            return false;
        }
    }

    bool interaction(ApplicationSystems& installed, InteractionView value) noexcept {
        const auto result = dispatch_interaction(installed, value);
        if (result.disposition == InteractionDispatchDisposition::ProtocolInvalid) {
            diagnostics.Emit([&] {
                return services::RuntimeDiagnosticFact{
                    .owner = contracts::DiagnosticOwner::BrowserRuntime,
                    .event = "browser.interaction.protocol_invalid",
                    .sequence = result.endpoint_id,
                };
            });
            return false;
        }
        if (result.disposition == InteractionDispatchDisposition::ApplicationRejected && result.error.has_value()) {
            const auto& error = result.error.value();
            if (result.endpoint_id == application_stable_id("annotation", "Input")) {
                std::scoped_lock lock(input_mutex);
                (void)publish_record(
                    InputProgress{.progress = {.epoch = input_epoch, .consumed_sequence = consumed_sequence}, .error = error},
                    transport::BrowserRecordPriority::Critical);
            } else {
                (void)publish_record(InteractionRejected{.endpoint_id = result.endpoint_id, .error = error},
                                     transport::BrowserRecordPriority::Critical);
            }
            diagnostics.Emit([&] {
                return services::RuntimeDiagnosticFact{
                    .owner = contracts::DiagnosticOwner::BrowserRuntime,
                    .event = "browser.interaction.rejected",
                    .participant = result.endpoint_name,
                    .sequence = result.endpoint_id,
                    .value = static_cast<std::uint64_t>(error.category),
                    .message = error.detail,
                };
            });
        } else if (result.disposition == InteractionDispatchDisposition::Accepted) {
            diagnostics.Emit([&] {
                return services::RuntimeDiagnosticFact{
                    .owner = contracts::DiagnosticOwner::BrowserRuntime,
                    .event = "browser.interaction.accepted",
                    .participant = result.endpoint_name,
                    .sequence = result.endpoint_id,
                    .value = result.generation,
                };
            });
        }
        return true;
    }

    void publish(SystemEvent event) noexcept {
        if (!admission.load(std::memory_order_acquire)) return;
        const auto record_priority = priority(event.delivery);
        (void)publish_record(event, record_priority);
    }
    void continuity_lost() noexcept {
        diagnostics.Emit([&] {
            return services::RuntimeDiagnosticFact{
                .owner = contracts::DiagnosticOwner::BrowserRuntime,
                .event = "browser.state_continuity_lost",
            };
        });
        if (server) server->close_peer();
    }

    static void Opened(void* context) noexcept { static_cast<Impl*>(context)->opened(); }
    static void Activated(void* context) noexcept { static_cast<Impl*>(context)->activated(); }
    static bool Record(void* context, const std::span<const std::byte> bytes) noexcept {
        return static_cast<Impl*>(context)->record(bytes);
    }
    void closed() noexcept {
        {
            std::scoped_lock lock(input_mutex);
            input_active = false;
        }
        if (!admission.load(std::memory_order_acquire)) return;
        auto* installed = systems.load(std::memory_order_acquire);
        if (installed != nullptr) {
            if (installed->presentation != nullptr) installed->presentation->SetApplicationPeerConnected(false);
            if (installed->annotation != nullptr) installed->annotation->PeerClosed();
        }
    }
    static void Closed(void* context) noexcept { static_cast<Impl*>(context)->closed(); }
    static void Diagnostic(void* context, const transport::BrowserServerEvent event, const std::size_t value) noexcept {
        auto& self = *static_cast<Impl*>(context);
        self.diagnostics.Emit([&] {
            return services::RuntimeDiagnosticFact{
                .owner = contracts::DiagnosticOwner::BrowserRuntime,
                .event = diagnostic_name(event),
                .value = value,
            };
        });
    }

    std::mutex input_mutex;
    std::uint64_t input_epoch = 0U;
    std::uint64_t consumed_sequence = 0U;
    bool input_active = false;
    transport::BrowserServer* server = nullptr;
    services::RuntimeDiagnosticTarget diagnostics;
    std::atomic<ApplicationSystems*> systems = nullptr;
    std::atomic_bool admission = true;
};

ApplicationBrowserHost::ApplicationBrowserHost(transport::BrowserServer& server, const services::RuntimeDiagnosticTarget diagnostics)
    : impl_(std::make_shared<Impl>(server, diagnostics)) {}

bool ApplicationBrowserHost::install(ApplicationSystems& systems) noexcept { return impl_->install(systems); }

transport::BrowserServer::Callbacks ApplicationBrowserHost::callbacks() const noexcept {
    return {
        .context = impl_,
        .opened = &Impl::Opened,
        .activated = &Impl::Activated,
        .record = &Impl::Record,
        .closed = &Impl::Closed,
        .diagnostic = impl_->diagnostics.valid() ? &Impl::Diagnostic : nullptr,
    };
}

void ApplicationBrowserHost::publish(SystemEvent event) noexcept { impl_->publish(std::move(event)); }
void ApplicationBrowserHost::continuity_lost() noexcept { impl_->continuity_lost(); }

void ApplicationBrowserHost::close_admission() noexcept { impl_->admission.store(false, std::memory_order_release); }

bool ApplicationBrowserHost::accepting() const noexcept { return impl_->admission.load(std::memory_order_acquire); }

}  // namespace mmltk::controller::browser
