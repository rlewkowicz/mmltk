#include "src/controller/contracts/application_systems.h"
#include "src/controller/browser/client_record.h"
#include "src/controller/browser/application_schema.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>
namespace mmltk::controller::browser {
namespace {
[[nodiscard]] std::unexpected<RecordCodecError> failure(const wire::ErrorCode code) { return std::unexpected(RecordCodecError{code}); }
[[nodiscard]] bool valid_intent(const Intent& intent) noexcept {
 return intent.protocol_version == kBrowserProtocolVersion && intent.correlation != 0U && intent.endpoint_id != 0U &&
        std::ranges::all_of(intent.fields, [&intent](const IntentField& field) {
         return field.field_id != 0U && std::ranges::count(intent.fields, field.field_id, &IntentField::field_id) == 1U;
        });
}
[[nodiscard]] bool valid_client_record(const ClientRecord& record) noexcept {
 return std::visit(
  []<class Record>(const Record& value) noexcept {
   using T = std::remove_cvref_t<Record>;
   if constexpr (std::same_as<T, Intent>) {
    return valid_intent(value);
   } else if constexpr (std::same_as<T, Interaction>) {
    return value.protocol_version == kBrowserProtocolVersion && value.endpoint_id != 0U;
   } else if constexpr (std::same_as<T, IntegrationControl>) {
    return value.protocol_version == kBrowserProtocolVersion && contracts::integration_receipt_valid(value.receipt) &&
           !contracts::integration_server_command(value.receipt.kind);
   }
  },
  record);
}
[[nodiscard]] bool valid_server_record(const ServerRecord& record) noexcept {
 return std::visit(
  []<class Record>(const Record& value) noexcept {
   using T = std::remove_cvref_t<Record>;
   if constexpr (std::same_as<T, Bootstrap>) {
    if (value.protocol_version != kBrowserProtocolVersion || value.input_epoch == 0U || value.schema_fingerprint[0] == 0U || value.schema_fingerprint[1] == 0U)
     return false;
    return std::ranges::all_of(value.snapshots, [&value](const SystemSnapshot& snapshot) {
     return snapshot.system_id != 0U && std::ranges::count(value.snapshots, snapshot.system_id, &SystemSnapshot::system_id) == 1U;
    });
   } else if constexpr (std::same_as<T, IntentReply>) {
    return value.protocol_version == kBrowserProtocolVersion && value.correlation != 0U && (value.result.has_value() != value.error.has_value());
   } else if constexpr (std::same_as<T, InteractionRejected>) {
    return value.protocol_version == kBrowserProtocolVersion && value.endpoint_id != 0U;
   } else if constexpr (std::same_as<T, IntegrationControl>) {
    return value.protocol_version == kBrowserProtocolVersion && contracts::integration_receipt_valid(value.receipt) &&
           contracts::integration_server_command(value.receipt.kind);
   } else {
    return value.protocol_version == kBrowserProtocolVersion && value.system_id != 0U && value.event_id != 0U;
   }
  },
  record);
}
[[nodiscard]] std::string normalized_error_detail(const char* detail) noexcept {
 try {
  const std::string_view source = detail == nullptr ? std::string_view{} : std::string_view(detail);
  std::string result;
  result.reserve(std::min(source.size(), kMaxErrorDetailBytes));
  for (const unsigned char byte : source) {
   if (result.size() == kMaxErrorDetailBytes) break;
   // Boundary diagnostics are deliberately normalized to printable
   // ASCII. This guarantees valid UTF-8 even for a broken dependency's
   // what() payload while preserving useful ordinary diagnostics.
   result.push_back(byte >= 0x20U && byte <= 0x7eU ? static_cast<char>(byte) : '?');
  }
  if (result.empty()) result = "application failure";
  return result;
 } catch (...) { return "application failure"; }
}
template <class Record>
[[nodiscard]] std::expected<void, RecordCodecError> encode_record(const Record& record, wire::ByteBuffer& destination, const wire::Limits limits) {
 auto encoded = mmltk::frameworks::serialization::encode(record, destination, limits);
 if (!encoded) return failure(encoded.error().code);
 return {};
}
template <class Record>
[[nodiscard]] std::expected<Record, RecordCodecError> decode_record(const wire::ByteSegments bytes, const wire::Limits limits) {
 auto decoded = mmltk::frameworks::serialization::decode<Record>(bytes, limits);
 if (!decoded) return failure(decoded.error().code);
 return std::move(*decoded);
}
[[nodiscard]] constexpr wire::Limits encoding_limits() noexcept {
 return {
  .max_bytes = kMaxRecordWireBytes,
  .max_items = kMaxRecordWireBytes,
  .max_depth = kMaxIntentValueDepth,
 };
}
[[nodiscard]] wire::Limits decoding_limits(const wire::ByteSegments bytes) noexcept {
 const std::size_t size = std::min(bytes.size(), kMaxRecordWireBytes);
 return {.max_bytes = size, .max_items = size, .max_depth = kMaxIntentValueDepth};
}
}  // namespace
bool is_interaction_record(const std::span<const std::byte> bytes) noexcept { return !bytes.empty() && (std::to_integer<unsigned>(bytes.front()) >> 5U) == 4U; }
namespace {
std::optional<InteractionView> decode_interaction_segments(const wire::ByteSegments bytes) {
 wire::Reader reader(bytes, decoding_limits(bytes));
 mmltk::frameworks::serialization::BorrowedByteRecord<CompactInteraction, &CompactInteraction::value> compact;
 if (!compact.DecodeCompact(reader)) return std::nullopt;
 std::uint64_t endpoint = 0U;
 visit_interaction_opcodes<ApplicationSystems>([&]<class Endpoint>(const auto opcode) {
  if (opcode == compact.Get<&CompactInteraction::opcode>()) endpoint = Endpoint::stable_id;
 });
 if (!endpoint) return std::nullopt;
 return InteractionView{Interaction{.endpoint_id = endpoint}, compact.Get<&CompactInteraction::value>()};
}
}  // namespace
std::optional<InteractionView> decode_interaction_view(const std::span<const std::byte> bytes) { return decode_interaction_segments({.first = bytes}); }
std::expected<void, RecordCodecError> encode_client_record(const ClientRecord& record, wire::ByteBuffer& destination) {
 if (!valid_client_record(record)) return failure(wire::ErrorCode::TypeMismatch);
 if (const auto* interaction = std::get_if<Interaction>(&record)) {
  std::optional<std::uint64_t> opcode;
  visit_interaction_opcodes<ApplicationSystems>([&]<class Endpoint>(const auto value) {
   if (interaction->endpoint_id == Endpoint::stable_id) opcode = value;
  });
  if (!opcode || interaction->value.size() > kMaxIntentValueBytes) return failure(wire::ErrorCode::TypeMismatch);
  using CompactWire = mmltk::frameworks::serialization::BorrowedByteRecord<CompactInteraction, &CompactInteraction::value>;
  destination.resize(CompactWire::EncodedCapacity(interaction->value.size()));
  mmltk::frameworks::serialization::FixedCborEncoder writer(destination);
  const CompactWire compact{CompactInteraction{.opcode = *opcode}, {.first = interaction->value}};
  if (!compact.EncodeCompact(writer)) return failure(wire::ErrorCode::LimitExceeded);
  destination.resize(writer.size());
  return {};
 }
 return encode_record(record, destination, encoding_limits());
}
std::expected<ClientRecord, RecordCodecError> decode_client_record(const wire::ByteSegments bytes) {
 // Owned diagnostics/fixtures use the same borrowed decoder as ingress.
 if (is_interaction_record(bytes.first.empty() ? bytes.second : bytes.first)) {
  auto view = decode_interaction_segments(bytes);
  if (!view) return failure(wire::ErrorCode::TypeMismatch);
  const auto payload = view->Get<&Interaction::value>();
  Interaction value{.endpoint_id = view->Get<&Interaction::endpoint_id>()};
  value.value.insert(value.value.end(), payload.first.begin(), payload.first.end());
  value.value.insert(value.value.end(), payload.second.begin(), payload.second.end());
  return ClientRecord{std::move(value)};
 }
 auto record = decode_record<ClientRecord>(bytes, decoding_limits(bytes));
 if (!record) return std::unexpected(record.error());
 if (std::holds_alternative<Interaction>(*record) || !valid_client_record(*record)) return failure(wire::ErrorCode::TypeMismatch);
 return std::move(*record);
}
std::expected<void, RecordCodecError> encode_server_record(const ServerRecord& record, wire::ByteBuffer& destination) {
 if (!valid_server_record(record)) return failure(wire::ErrorCode::TypeMismatch);
 return encode_record(record, destination, encoding_limits());
}
std::expected<ServerRecord, RecordCodecError> decode_server_record(const wire::ByteSegments bytes) {
 auto record = decode_record<ServerRecord>(bytes, decoding_limits(bytes));
 if (!record) return std::unexpected(record.error());
 if (!valid_server_record(*record)) return failure(wire::ErrorCode::TypeMismatch);
 return std::move(*record);
}
ApplicationErrorRecord map_current_exception() noexcept {
 try {
  throw;
 } catch (const mmltk::controller::contracts::ApplicationError& error) {
  return {.category = error.category(), .detail = normalized_error_detail(error.what())};
 } catch (const std::invalid_argument& error) {
  return {.category = mmltk::controller::contracts::ApplicationErrorCategory::InvalidIntent, .detail = normalized_error_detail(error.what())};
 } catch (const std::exception& error) {
  return {.category = mmltk::controller::contracts::ApplicationErrorCategory::Failed, .detail = normalized_error_detail(error.what())};
 } catch (...) { return {.category = mmltk::controller::contracts::ApplicationErrorCategory::Failed, .detail = "unknown application failure"}; }
}
}  // namespace mmltk::controller::browser
