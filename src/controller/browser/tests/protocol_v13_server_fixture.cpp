#include "src/controller/contracts/application_systems.h"
#include "src/controller/browser/application_schema.h"
#include "src/controller/browser/application_materializer.h"

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
    auto snapshot = mmltk::frameworks::serialization::reflected_value(contracts::SettingsUiState{});
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
                .path = "/tmp/protocol-v13-fixture",
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
    if (!snapshot || !cancelled_reply || !selected_reply || !event) return EXIT_FAILURE;
    const std::array<ServerRecord, 4U> records{
        Bootstrap{.schema_fingerprint = application_schema_fingerprint<ApplicationSystems>().words,
                  .snapshots = {{
                      .system_id = application_system_stable_id<&ApplicationSystems::settings>(),
                      .value = std::move(*snapshot),
                  }}},
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
    };
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
