#include "src/controller/contracts/application_systems.h"
#include "src/controller/browser/application_schema.h"
#include "src/controller/browser/application_materializer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

int main(const int argument_count, char* const* const arguments) {
    using namespace mmltk::controller;
    using namespace mmltk::controller::browser;
    if (argument_count != 2 || arguments[1] == nullptr || std::string_view(arguments[1]).empty()) return EXIT_FAILURE;

    std::size_t system_count = 0U;
    std::size_t intent_count = 0U;
    std::size_t interaction_count = 0U;
    std::size_t event_count = 0U;
    ApplicationSchema<ApplicationSystems>::VisitSystems([&]<class SystemCell, std::meta::info Snapshot>() {
        ++system_count;
        constexpr auto annotation = application_schema_detail::annotation_value<Snapshot, contracts::reflection::Snapshot>();
        static_assert(annotation.byte_budget != 0U);
    });
    ApplicationSchema<ApplicationSystems>::VisitEndpoints([&]<class Endpoint>() {
        if constexpr (Endpoint::interaction)
            ++interaction_count;
        else
            ++intent_count;
    });
    ApplicationSchema<ApplicationSystems>::VisitEvents([&]<class Identity, class Event>(const contracts::reflection::Event annotation) {
        ++event_count;
        if (!mmltk::frameworks::reflection::enum_contains(annotation.delivery)) event_count = 0U;
    });
    if (system_count != 13U || intent_count == 0U || interaction_count == 0U || event_count == 0U) return EXIT_FAILURE;

    using SettingsEvent = ApplicationEventIdentity<ApplicationSystems, &ApplicationSystems::settings, SettingsChanged>;
    Bootstrap bootstrap{.schema_fingerprint = application_schema_fingerprint<ApplicationSystems>().words, .snapshots = {}};
    bool snapshots_valid = true;
    ApplicationSchema<ApplicationSystems>::VisitSnapshotDefaults([&]<class Cell, class Snapshot>(Snapshot snapshot) {
        if constexpr (requires { typename Cell::type::visual_source; }) {
            using Projection = typename Cell::type::visual_source;
            const auto session = presentation_source_session(Projection::kind);
            auto& frame = mmltk::frameworks::reflection::access<Snapshot, Projection::frame>(snapshot);
            frame = visual_frame({Projection::kind, 1U}, {32U, 24U}, 7U + session);
            frame.content = {1U, 2U, 20U, 16U};
            frame.clean_revision = 43U;
            mmltk::frameworks::reflection::access<Snapshot, Projection::revision>(snapshot) =
                std::numeric_limits<std::uint64_t>::max() - session;
            snapshots_valid = snapshots_valid && visual_clean_content_identity(frame).revision == 43U;
        }
        auto encoded = mmltk::frameworks::serialization::reflected_value(snapshot);
        if (!encoded) {
            snapshots_valid = false;
            return;
        }
        bootstrap.snapshots.push_back({.system_id = Cell::stable_id, .value = std::move(*encoded)});
    });
    if (!snapshots_valid) return EXIT_FAILURE;
    std::uint64_t dialog_id = 0U;
    ApplicationSchema<ApplicationSystems>::VisitApplicationSettingsLeaves(
        [&]<class Owner, class Declaration, class Member>(const ApplicationSettingsLeafFact& fact) {
            if (dialog_id == 0U && fact.file_dialog) dialog_id = fact.stable_id;
        });
    if (dialog_id == 0U) return EXIT_FAILURE;
    const services::FileDialogSelection cancelled{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{dialog_id}},
        .result = services::FileDialogCancelled{},
    };
    const services::FileDialogSelection selected{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{dialog_id}},
        .result =
            services::FileDialogSelected{
                .path = "/tmp/protocol-v15-fixture",
            },
    };
    auto cancelled_reply = mmltk::frameworks::serialization::reflected_value(FileDialogSnapshot{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{dialog_id}},
        .selection = cancelled,
    });
    auto selected_reply = mmltk::frameworks::serialization::reflected_value(FileDialogSnapshot{
        .target = services::FileDialogTarget{services::SettingsFieldTarget{dialog_id}},
        .selection = selected,
    });
    auto event = mmltk::frameworks::serialization::reflected_value(SettingsChanged{.snapshot = contracts::SettingsUiState{}});
    if (!cancelled_reply || !selected_reply || !event) return EXIT_FAILURE;
    bootstrap.input_epoch = 1U;
    std::vector<ServerRecord> records{
        std::move(bootstrap),
        IntentReply{
            .correlation = 17U,
            .result = std::move(*cancelled_reply),
            .error = {},
        },
        IntentReply{
            .correlation = 18U,
            .result = std::move(*selected_reply),
            .error = {},
        },
        SystemEvent{
            .system_id = SettingsEvent::system_id,
            .event_id = SettingsEvent::event_id,
            .delivery = contracts::reflection::EventDelivery::Critical,
            .value = std::move(*event),
        },
        InputProgress{.progress = {.epoch = 1U, .consumed_sequence = 2U}},
        InteractionRejected{.endpoint_id = application_stable_id("explore", "UpdateViewport"),
                            .error = {.category = contracts::ApplicationErrorCategory::Unavailable, .detail = "fixture unavailable"}},
        InputProgress{.progress = {.epoch = 1U, .consumed_sequence = 2U, .rejection = std::string(kVisualFailureByteCapacity, 'r')},
                      .error = ApplicationErrorRecord{.category = contracts::ApplicationErrorCategory::Busy, .detail = "fixture busy"}},
        IntegrationControl{.receipt = {.kind = contracts::IntegrationControlKind::Advance, .sequence = 2U}},
    };
    using ControlKind = contracts::IntegrationControlKind;
    template for (constexpr auto enumerator : std::define_static_array(std::meta::enumerators_of(^^ControlKind))) {
        constexpr auto kind = std::meta::extract<ControlKind>(enumerator);
        constexpr auto policy = contracts::integration_command_direction<kind>();
        if constexpr (policy.server && kind != ControlKind::Advance) {
            records.emplace_back(IntegrationControl{.receipt = {.kind = kind, .sequence = 2U,
                .read_generation = policy.read_generation ? 7U : 0U, .compiled_index = 0U}});
        }
    }
    bool complete_record_surface = true;
    application_schema_detail::Variant<ServerRecord>::Visit([&]<class Alternative>() {
        complete_record_surface = complete_record_surface && std::ranges::any_of(records, [](const ServerRecord& record) {
                                      return std::holds_alternative<Alternative>(record);
                                  });
    });
    if (!complete_record_surface) return EXIT_FAILURE;
    std::ofstream output(arguments[1], std::ios::binary | std::ios::trunc);
    if (!output) return EXIT_FAILURE;
    for (const ServerRecord& record : records) {
        wire::ByteBuffer encoded;
        if (!encode_server_record(record, encoded) || encoded.size() > std::numeric_limits<std::uint32_t>::max()) return EXIT_FAILURE;
        const auto size = static_cast<std::uint32_t>(encoded.size());
        const std::array header{
            static_cast<unsigned char>(size >> 24U),
            static_cast<unsigned char>(size >> 16U),
            static_cast<unsigned char>(size >> 8U),
            static_cast<unsigned char>(size),
        };
        output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
        output.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()));
    }
    return output ? EXIT_SUCCESS : EXIT_FAILURE;
}
