#pragma once
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/controller/contracts/application_boundary.h"
#include "src/controller/contracts/integration_control.h"
#include "src/controller/presentation/visual_system_types.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::controller::browser {
namespace field_policy = mmltk::frameworks::reflection;
namespace wire = mmltk::frameworks::serialization::wire;
inline constexpr std::uint64_t kBrowserProtocolVersion = 17U;
// Aggregate output admission ceilings. Dynamic values remain actual-sized;
// individual input fields retain the independent intent limits below.
inline constexpr std::size_t kMaxOutputValueBytes = 8U * 1024U * 1024U;
inline constexpr std::size_t kMaxOutputValueItems = kMaxOutputValueBytes;
inline constexpr std::size_t kMaxRecordWireBytes = 32U * 1024U * 1024U;
inline constexpr std::size_t kMaxIntentValueBytes = 65536U;
inline constexpr std::size_t kMaxIntentValueItems = 1024U;
inline constexpr std::size_t kMaxIntentValueDepth = 64U;
inline constexpr std::size_t kMaxSnapshotCount = 32U;
inline constexpr std::size_t kMaxErrorDetailBytes = 512U;
inline constexpr std::size_t kMaxIntentFields = 64U;
static_assert(kMaxIntentValueDepth == wire::kMaximumNestingDepth);
struct IntentField final {
 [[= field_policy::Minimum{std::uint64_t{1U}}]] std::uint64_t field_id = 0U;
 [[= field_policy::MaxBytes{kMaxIntentValueBytes}]][[= field_policy::MaxItems{kMaxIntentValueItems}]] wire::Value
  value{};  // CLEANUP-IGNORE: Intent fields and System events are separate dynamic-value wire records.
 bool operator==(const IntentField&) const = default;
};
struct Intent final {
 std::uint64_t protocol_version = kBrowserProtocolVersion;
 [[= field_policy::Minimum{std::uint64_t{1U}}]] std::uint64_t correlation = 0U;
 [[= field_policy::Minimum{std::uint64_t{1U}}]] std::uint64_t endpoint_id = 0U;
 [[= field_policy::MaxItems{kMaxIntentFields}]] std::vector<IntentField> fields;
 bool operator==(const Intent&) const = default;
};
struct Interaction final {
 std::uint64_t protocol_version = kBrowserProtocolVersion;
 [[= field_policy::Minimum{std::uint64_t{1U}}]] std::uint64_t endpoint_id = 0U;
 [[= field_policy::MaxBytes{kMaxIntentValueBytes}]] wire::ByteBuffer value{};
 bool operator==(const Interaction&) const = default;
};
// Connection bootstrap establishes protocol/schema identity. Interaction opcodes
// are projected from the canonical endpoint order, never a parallel registry.
struct CompactInteraction final {
 std::uint64_t opcode = 0U;
 [[= field_policy::MaxBytes{kMaxIntentValueBytes}]] wire::ByteBuffer value{};
};
MMLTK_REFLECT_FIELDS(CompactInteraction)
// CLEANUP-IGNORE: System snapshots and intent fields are distinct wire records with different byte limits and stable identities.
struct SystemSnapshot final {
 [[= field_policy::Minimum{std::uint64_t{1U}}]] std::uint64_t system_id = 0U;
 [[= field_policy::MaxBytes{kMaxOutputValueBytes}]][[= field_policy::MaxItems{kMaxOutputValueItems}]] wire::Value value{};
 bool operator==(const SystemSnapshot&) const = default;
};
// Opaque to Firefox. The source projection captures this value against the
// retained product before display finalization; browser UI snapshots are
// neither the source of these facts nor a condition for drawing them.
struct WorkspaceImageMetadata final {
 std::array<std::uint64_t, 2U> schema_fingerprint{};
 VisualFrame frame{};
 SystemSnapshot product{};
 std::optional<SystemSnapshot> source{};
};
struct Bootstrap final {
 std::uint64_t protocol_version = kBrowserProtocolVersion;
 std::array<std::uint64_t, 2U> schema_fingerprint{};
 std::uint64_t input_epoch = 0U;
 [[= field_policy::MaxItems{kMaxSnapshotCount}]] std::vector<SystemSnapshot> snapshots;
 bool operator==(const Bootstrap&) const = default;
};
struct ApplicationErrorRecord final {
 mmltk::controller::contracts::ApplicationErrorCategory category = mmltk::controller::contracts::ApplicationErrorCategory::Failed;
 // CLEANUP-IGNORE: Error detail and reply payload apply different wire types despite sharing the envelope byte
 // budget.
 [[= field_policy::MaxBytes{kMaxErrorDetailBytes}]] std::string detail;
 bool operator==(const ApplicationErrorRecord&) const = default;
};
struct IntentReply final {
 std::uint64_t protocol_version = kBrowserProtocolVersion;
 [[= field_policy::Minimum{std::uint64_t{1U}}]] std::uint64_t correlation = 0U;
 [[= field_policy::MaxBytes{kMaxOutputValueBytes}]][[= field_policy::MaxItems{kMaxOutputValueItems}]] std::optional<wire::Value> result;
 std::optional<ApplicationErrorRecord> error{};
 bool operator==(const IntentReply&) const = default;
};
struct SystemEvent final {
 std::uint64_t protocol_version = kBrowserProtocolVersion;
 [[= field_policy::Minimum{std::uint64_t{1U}}]] std::uint64_t system_id = 0U;
 [[= field_policy::Minimum{std::uint64_t{1U}}]] std::uint64_t event_id = 0U;
 mmltk::controller::contracts::reflection::EventDelivery delivery = mmltk::controller::contracts::reflection::EventDelivery::Transient;
 std::uint64_t state_revision = 0U;
 [[= field_policy::MaxBytes{kMaxOutputValueBytes}]][[= field_policy::MaxItems{kMaxOutputValueItems}]] wire::Value value{};
 bool operator==(const SystemEvent&) const = default;
};
struct InteractionRejected final {
 bool operator==(const InteractionRejected&) const = default;
 std::uint64_t protocol_version = kBrowserProtocolVersion;
 std::uint64_t endpoint_id = 0U;
 ApplicationErrorRecord error{};
};
MMLTK_REFLECT_FIELDS(InteractionRejected)
struct IntegrationControl final {
 std::uint64_t protocol_version = kBrowserProtocolVersion;
 contracts::IntegrationControlReceipt receipt{};
 bool operator==(const IntegrationControl&) const = default;
};
MMLTK_REFLECT_FIELDS(IntegrationControl)
using ClientRecord = std::variant<Intent, Interaction, IntegrationControl>;
using ServerRecord = std::variant<Bootstrap, IntentReply, SystemEvent, InteractionRejected, IntegrationControl>;
MMLTK_REFLECT_FIELDS(IntentField)
MMLTK_REFLECT_FIELDS(Intent)
MMLTK_REFLECT_FIELDS(Interaction)
MMLTK_REFLECT_FIELDS(SystemSnapshot)
MMLTK_REFLECT_FIELDS(WorkspaceImageMetadata)
MMLTK_REFLECT_FIELDS(Bootstrap)
MMLTK_REFLECT_FIELDS(ApplicationErrorRecord)
MMLTK_REFLECT_FIELDS(IntentReply)
MMLTK_REFLECT_FIELDS(SystemEvent)
using InteractionView = mmltk::frameworks::serialization::BorrowedByteRecord<Interaction, &Interaction::value>;
[[nodiscard]] bool is_interaction_record(std::span<const std::byte>) noexcept;
[[nodiscard]] std::optional<InteractionView> decode_interaction_view(std::span<const std::byte>);
struct RecordCodecError final {
 wire::ErrorCode code = wire::ErrorCode::MalformedItem;
};
[[nodiscard]] std::expected<void, RecordCodecError> encode_client_record(const ClientRecord& record, wire::ByteBuffer& destination);
[[nodiscard]] std::expected<ClientRecord, RecordCodecError> decode_client_record(wire::ByteSegments bytes);
[[nodiscard]] std::expected<void, RecordCodecError> encode_server_record(const ServerRecord& record, wire::ByteBuffer& destination);
[[nodiscard]] std::expected<ServerRecord, RecordCodecError> decode_server_record(wire::ByteSegments bytes);
[[nodiscard]] ApplicationErrorRecord map_current_exception() noexcept;
}  // namespace mmltk::controller::browser
