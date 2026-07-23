#pragma once

#include "browser/host_api_protocol.h"
#include "mmltk/live/live_frame_id.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

namespace mmltk::browser {

inline constexpr std::string_view kLinuxImportableImageHandleTypeNativeToken = "native_token";

enum class FramePixelFormat : std::uint8_t {
    Bgr8 = 0,
    Rgba8 = 1,
};

enum class FrameTransportKind : std::uint8_t {
    CudaDeviceBuffer = 0,
};

enum class FrameSlotLifecycle : std::uint8_t {
    SnapshotRetained = 0,
    Persistent = 1,
    ExplicitRelease = 2,
};

enum class FrameSlotOwnership : std::uint8_t {
    NativeHost = 0,
    Shared = 1,
    Browser = 2,
};

enum class FrameSlotSyncKind : std::uint8_t {
    CpuReady = 0,
    BinaryFence = 1,
    TimelinePoint = 2,
};

enum class LinuxImportableImageHandleKind : std::uint8_t {
    Unknown = 0,
    OpaqueToken = 1,
};

enum class FrameSlotNativeImportReleaseBehavior : std::uint8_t {
    Unknown = 0,
    TransportMessage = 1,
    ContractRelease = 2,
};

struct FrameSlotReadySync {
    FrameSlotSyncKind kind = FrameSlotSyncKind::CpuReady;
    std::string handle;
    std::uint64_t value = 0;

    bool operator==(const FrameSlotReadySync&) const noexcept = default;
};

struct FrameSlotNativeImportMetadata {
    std::string runtime;
    std::string texture_format;
    FrameSlotNativeImportReleaseBehavior release_behavior = FrameSlotNativeImportReleaseBehavior::Unknown;

    bool operator==(const FrameSlotNativeImportMetadata&) const noexcept = default;
};

struct LinuxImportableImageHandle {
    LinuxImportableImageHandleKind kind = LinuxImportableImageHandleKind::Unknown;
    std::string handle_type;
    std::string handle;

    bool operator==(const LinuxImportableImageHandle&) const noexcept = default;
};

struct LinuxImportableFrameSlotContract {
    LinuxImportableFrameSlotContract() = default;
    LinuxImportableFrameSlotContract(LinuxImportableImageHandle image_in, FrameSlotReadySync ready_sync_in,
                                     std::string release_token_in,
                                     std::optional<FrameSlotNativeImportMetadata> metadata_in = {})
        : image(std::move(image_in)),
          ready_sync(std::move(ready_sync_in)),
          release_token(std::move(release_token_in)),
          metadata(std::move(metadata_in)) {}

    LinuxImportableImageHandle image{};
    FrameSlotReadySync ready_sync{};
    std::string release_token;
    std::optional<FrameSlotNativeImportMetadata> metadata;

    bool operator==(const LinuxImportableFrameSlotContract&) const noexcept = default;
};

struct FrameSlotDescriptor {
    std::string slot_name;
    FrameTransportKind transport = FrameTransportKind::CudaDeviceBuffer;
    FramePixelFormat pixel_format = FramePixelFormat::Bgr8;
    std::uint32_t slot_index = 0;
    mmltk::live::LiveFrameId frame_id{};
    CaptureRegion capture_region{};
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t row_stride_bytes = 0;
    std::uint64_t byte_length = 0;
    std::uint64_t ready_ns = 0;
    bool short_frame = false;
    std::optional<LinuxImportableFrameSlotContract> linux_import;
    FrameSlotLifecycle lifecycle = FrameSlotLifecycle::SnapshotRetained;
    FrameSlotOwnership ownership = FrameSlotOwnership::NativeHost;
    FrameSlotReadySync ready_sync{};
};

namespace frame_slot_contract_detail {

template <typename T>
inline void get_optional_alias(const nlohmann::json& j, const char* snake_key, const char* camel_key, T& out) {
    if (j.contains(snake_key)) {
        j.at(snake_key).get_to(out);
        return;
    }
    if (j.contains(camel_key)) {
        j.at(camel_key).get_to(out);
    }
}

[[nodiscard]] inline nlohmann::json::const_iterator find_optional_member(
    const nlohmann::json& j, const std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        if (const auto it = j.find(key); it != j.end() && !it->is_null()) {
            return it;
        }
    }
    return j.end();
}

[[noreturn]] inline void throw_invalid_enum(const char* kind, const std::string_view value) {
    throw std::runtime_error("invalid browser frame slot " + std::string(kind) + ": " + std::string(value));
}

inline void to_json_live_frame_id(nlohmann::json& j, const mmltk::live::LiveFrameId& frame_id) {
    j = nlohmann::json{
        {"session_nonce", frame_id.session_nonce},
        {"sequence", frame_id.sequence},
    };
}

inline void from_json_live_frame_id(const nlohmann::json& j, mmltk::live::LiveFrameId& frame_id) {
    j.at("session_nonce").get_to(frame_id.session_nonce);
    j.at("sequence").get_to(frame_id.sequence);
}

}  

[[nodiscard]] inline std::string_view frame_pixel_format_name(const FramePixelFormat format) noexcept {
    switch (format) {
        case FramePixelFormat::Bgr8:
            return "bgr8";
        case FramePixelFormat::Rgba8:
            return "rgba8";
    }
    return "bgr8";
}

[[nodiscard]] inline FramePixelFormat frame_pixel_format_from_name(const std::string_view name) {
    if (name == "bgr8") {
        return FramePixelFormat::Bgr8;
    }
    if (name == "rgba8") {
        return FramePixelFormat::Rgba8;
    }
    frame_slot_contract_detail::throw_invalid_enum("pixel format", name);
}

[[nodiscard]] inline std::string_view frame_transport_name(const FrameTransportKind transport) noexcept {
    switch (transport) {
        case FrameTransportKind::CudaDeviceBuffer:
            return "cuda_device_buffer";
    }
    return "cuda_device_buffer";
}

[[nodiscard]] inline FrameTransportKind frame_transport_from_name(const std::string_view name) {
    if (name == "cuda_device_buffer" || name == "cudaDeviceBuffer") {
        return FrameTransportKind::CudaDeviceBuffer;
    }
    frame_slot_contract_detail::throw_invalid_enum("transport", name);
}

[[nodiscard]] inline std::string_view frame_slot_lifecycle_name(const FrameSlotLifecycle lifecycle) noexcept {
    switch (lifecycle) {
        case FrameSlotLifecycle::SnapshotRetained:
            return "snapshot_retained";
        case FrameSlotLifecycle::Persistent:
            return "persistent";
        case FrameSlotLifecycle::ExplicitRelease:
            return "explicit_release";
    }
    return "snapshot_retained";
}

[[nodiscard]] inline FrameSlotLifecycle frame_slot_lifecycle_from_name(const std::string_view name) {
    if (name == "snapshot_retained" || name == "snapshotRetained") {
        return FrameSlotLifecycle::SnapshotRetained;
    }
    if (name == "persistent") {
        return FrameSlotLifecycle::Persistent;
    }
    if (name == "explicit_release" || name == "explicitRelease") {
        return FrameSlotLifecycle::ExplicitRelease;
    }
    frame_slot_contract_detail::throw_invalid_enum("lifecycle", name);
}

[[nodiscard]] inline std::string_view frame_slot_ownership_name(const FrameSlotOwnership ownership) noexcept {
    switch (ownership) {
        case FrameSlotOwnership::NativeHost:
            return "native_host";
        case FrameSlotOwnership::Shared:
            return "shared";
        case FrameSlotOwnership::Browser:
            return "browser";
    }
    return "native_host";
}

[[nodiscard]] inline FrameSlotOwnership frame_slot_ownership_from_name(const std::string_view name) {
    if (name == "native_host" || name == "nativeHost") {
        return FrameSlotOwnership::NativeHost;
    }
    if (name == "shared") {
        return FrameSlotOwnership::Shared;
    }
    if (name == "browser") {
        return FrameSlotOwnership::Browser;
    }
    frame_slot_contract_detail::throw_invalid_enum("ownership", name);
}

[[nodiscard]] inline std::string_view frame_slot_sync_kind_name(const FrameSlotSyncKind kind) noexcept {
    switch (kind) {
        case FrameSlotSyncKind::CpuReady:
            return "cpu_ready";
        case FrameSlotSyncKind::BinaryFence:
            return "binary_fence";
        case FrameSlotSyncKind::TimelinePoint:
            return "timeline_point";
    }
    return "cpu_ready";
}

[[nodiscard]] inline FrameSlotSyncKind frame_slot_sync_kind_from_name(const std::string_view name) {
    if (name == "cpu_ready" || name == "cpuReady") {
        return FrameSlotSyncKind::CpuReady;
    }
    if (name == "binary_fence" || name == "binaryFence") {
        return FrameSlotSyncKind::BinaryFence;
    }
    if (name == "timeline_point" || name == "timelinePoint") {
        return FrameSlotSyncKind::TimelinePoint;
    }
    frame_slot_contract_detail::throw_invalid_enum("sync kind", name);
}

[[nodiscard]] inline std::string_view frame_slot_native_import_release_behavior_name(
    const FrameSlotNativeImportReleaseBehavior behavior) noexcept {
    switch (behavior) {
        case FrameSlotNativeImportReleaseBehavior::Unknown:
            return "unknown";
        case FrameSlotNativeImportReleaseBehavior::TransportMessage:
            return "transport_message";
        case FrameSlotNativeImportReleaseBehavior::ContractRelease:
            return "contract_release";
    }
    return "unknown";
}

[[nodiscard]] inline FrameSlotNativeImportReleaseBehavior frame_slot_native_import_release_behavior_from_name(
    const std::string_view name) {
    if (name == "unknown") {
        return FrameSlotNativeImportReleaseBehavior::Unknown;
    }
    if (name == "transport_message" || name == "transportMessage") {
        return FrameSlotNativeImportReleaseBehavior::TransportMessage;
    }
    if (name == "contract_release" || name == "contractRelease") {
        return FrameSlotNativeImportReleaseBehavior::ContractRelease;
    }
    frame_slot_contract_detail::throw_invalid_enum("native import release behavior", name);
}

[[nodiscard]] inline std::string_view linux_importable_image_handle_kind_name(
    const LinuxImportableImageHandleKind kind) noexcept {
    switch (kind) {
        case LinuxImportableImageHandleKind::Unknown:
            return "unknown";
        case LinuxImportableImageHandleKind::OpaqueToken:
            return "opaque_token";
    }
    return "unknown";
}

[[nodiscard]] inline LinuxImportableImageHandleKind linux_importable_image_handle_kind_from_name(
    const std::string_view name) {
    if (name == "unknown") {
        return LinuxImportableImageHandleKind::Unknown;
    }
    if (name == "opaque_token" || name == "opaqueToken") {
        return LinuxImportableImageHandleKind::OpaqueToken;
    }
    frame_slot_contract_detail::throw_invalid_enum("image handle kind", name);
}

[[nodiscard]] constexpr std::uint64_t frame_byte_length(const std::uint64_t row_stride_bytes,
                                                        const std::uint32_t height) noexcept {
    return row_stride_bytes * static_cast<std::uint64_t>(height);
}

[[nodiscard]] inline bool frame_slot_ready_sync_is_cpu_ready(const FrameSlotReadySync& sync) noexcept {
    return sync.kind == FrameSlotSyncKind::CpuReady && sync.handle.empty() && sync.value == 0U;
}

[[nodiscard]] inline bool linux_importable_frame_slot_contract_is_empty(
    const LinuxImportableFrameSlotContract& contract) noexcept {
    return contract.image.handle_type.empty() && contract.image.handle.empty() && contract.release_token.empty() &&
           frame_slot_ready_sync_is_cpu_ready(contract.ready_sync);
}

[[nodiscard]] inline bool frame_slot_native_import_metadata_is_empty(
    const FrameSlotNativeImportMetadata& metadata) noexcept {
    return metadata.runtime.empty() && metadata.texture_format.empty() &&
           metadata.release_behavior == FrameSlotNativeImportReleaseBehavior::Unknown;
}

[[nodiscard]] inline std::optional<FrameSlotNativeImportMetadata> canonical_frame_slot_native_import_metadata(
    const FramePixelFormat pixel_format, const std::string_view release_token,
    const std::optional<FrameSlotNativeImportMetadata>& structured_metadata) noexcept {
    FrameSlotNativeImportMetadata metadata{};

    if (structured_metadata.has_value() && !frame_slot_native_import_metadata_is_empty(*structured_metadata)) {
        metadata = *structured_metadata;
    }

    if (pixel_format != FramePixelFormat::Rgba8 && metadata.texture_format == "rgba8unorm") {
        metadata.texture_format.clear();
    }
    if (metadata.texture_format.empty() && pixel_format == FramePixelFormat::Rgba8) {
        metadata.texture_format = "rgba8unorm";
    }
    if (release_token.empty() && metadata.release_behavior == FrameSlotNativeImportReleaseBehavior::TransportMessage) {
        metadata.release_behavior = FrameSlotNativeImportReleaseBehavior::Unknown;
    }
    if (metadata.release_behavior == FrameSlotNativeImportReleaseBehavior::Unknown && !release_token.empty()) {
        metadata.release_behavior = FrameSlotNativeImportReleaseBehavior::TransportMessage;
    }

    if (frame_slot_native_import_metadata_is_empty(metadata)) {
        return std::nullopt;
    }
    return metadata;
}

inline void normalize_linux_importable_image_handle(LinuxImportableImageHandle& image) noexcept {
    if (image.kind == LinuxImportableImageHandleKind::Unknown && !image.handle_type.empty()) {
        image.kind = LinuxImportableImageHandleKind::OpaqueToken;
    }
    if (image.kind == LinuxImportableImageHandleKind::OpaqueToken && image.handle_type.empty()) {
        image.handle_type = std::string(kLinuxImportableImageHandleTypeNativeToken);
    }
}

[[nodiscard]] inline std::optional<LinuxImportableFrameSlotContract> frame_slot_linux_import_contract(
    const FrameSlotDescriptor& descriptor) noexcept {
    if (!descriptor.linux_import.has_value() ||
        linux_importable_frame_slot_contract_is_empty(*descriptor.linux_import)) {
        return std::nullopt;
    }
    LinuxImportableFrameSlotContract contract = *descriptor.linux_import;
    normalize_linux_importable_image_handle(contract.image);
    contract.metadata =
        canonical_frame_slot_native_import_metadata(descriptor.pixel_format, contract.release_token, contract.metadata);
    return contract;
}

[[nodiscard]] inline std::optional<FrameSlotNativeImportMetadata> frame_slot_native_import_metadata(
    const FrameSlotDescriptor& descriptor) noexcept {
    const std::optional<LinuxImportableFrameSlotContract> contract = frame_slot_linux_import_contract(descriptor);
    if (!contract.has_value()) {
        return std::nullopt;
    }
    return contract->metadata;
}

inline void normalize_frame_slot_native_import_metadata(FrameSlotDescriptor& descriptor) noexcept;

inline void apply_frame_slot_native_import_metadata(FrameSlotDescriptor& descriptor,
                                                    const FrameSlotNativeImportMetadata& metadata) noexcept {
    if (!descriptor.linux_import.has_value()) {
        return;
    }
    auto& linux_import = *descriptor.linux_import;
    const std::optional<LinuxImportableFrameSlotContract> contract = frame_slot_linux_import_contract(descriptor);
    if (!contract || frame_slot_native_import_metadata_is_empty(metadata)) {
        linux_import.metadata.reset();
        return;
    }
    linux_import.metadata = metadata;
    normalize_frame_slot_native_import_metadata(descriptor);
}

inline void normalize_frame_slot_native_import_metadata(FrameSlotDescriptor& descriptor) noexcept {
    if (descriptor.linux_import.has_value()) {
        descriptor.linux_import->metadata = frame_slot_native_import_metadata(descriptor);
    }
}

inline void apply_linux_importable_frame_slot_contract(FrameSlotDescriptor& descriptor,
                                                       LinuxImportableFrameSlotContract contract) noexcept {
    normalize_linux_importable_image_handle(contract.image);

    descriptor.linux_import = linux_importable_frame_slot_contract_is_empty(contract)
                                  ? std::nullopt
                                  : std::optional<LinuxImportableFrameSlotContract>(std::move(contract));
    if (!descriptor.linux_import.has_value()) {
        descriptor.ready_sync = FrameSlotReadySync{};
        return;
    }

    descriptor.ready_sync = descriptor.linux_import->ready_sync;
    normalize_frame_slot_native_import_metadata(descriptor);
}

inline void normalize_linux_importable_frame_slot_contract(FrameSlotDescriptor& descriptor) noexcept {
    const std::optional<LinuxImportableFrameSlotContract> contract = frame_slot_linux_import_contract(descriptor);
    if (!contract.has_value()) {
        descriptor.linux_import.reset();
        descriptor.ready_sync = FrameSlotReadySync{};
        return;
    }
    apply_linux_importable_frame_slot_contract(descriptor, *contract);
}

[[nodiscard]] inline bool frame_slot_has_valid_byte_layout(const FrameSlotDescriptor& descriptor) noexcept {
    return descriptor.width > 0U && descriptor.height > 0U && descriptor.row_stride_bytes > 0U &&
           descriptor.byte_length == frame_byte_length(descriptor.row_stride_bytes, descriptor.height);
}

[[nodiscard]] inline bool frame_slot_supports_imported_acquire(const FrameSlotDescriptor& descriptor) noexcept {
    const std::optional<LinuxImportableFrameSlotContract> contract = frame_slot_linux_import_contract(descriptor);
    return descriptor.transport == FrameTransportKind::CudaDeviceBuffer &&
           frame_slot_has_valid_byte_layout(descriptor) && contract.has_value() &&
           contract->image.kind == LinuxImportableImageHandleKind::OpaqueToken &&
           !contract->image.handle_type.empty() && !contract->image.handle.empty() &&
           contract->image.handle != contract->ready_sync.handle &&
           descriptor.lifecycle == FrameSlotLifecycle::ExplicitRelease &&
           descriptor.ownership == FrameSlotOwnership::NativeHost &&
           contract->ready_sync.kind != FrameSlotSyncKind::CpuReady && !contract->ready_sync.handle.empty() &&
           !contract->release_token.empty() && contract->release_token != contract->image.handle &&
           contract->release_token != contract->ready_sync.handle;
}

inline void to_json(nlohmann::json& j, const FrameSlotReadySync& sync) {
    j = nlohmann::json{
        {"kind", frame_slot_sync_kind_name(sync.kind)},
        {"handle", sync.handle},
        {"value", sync.value},
    };
}

inline void from_json(const nlohmann::json& j, FrameSlotReadySync& sync) {
    sync = FrameSlotReadySync{};
    sync.kind = frame_slot_sync_kind_from_name(j.at("kind").get<std::string>());
    frame_slot_contract_detail::get_optional_alias(j, "handle", "handle", sync.handle);
    frame_slot_contract_detail::get_optional_alias(j, "value", "value", sync.value);
}

inline void to_json(nlohmann::json& j, const FrameSlotNativeImportMetadata& metadata) {
    j = nlohmann::json::object();
    if (!metadata.runtime.empty()) {
        j["runtime"] = metadata.runtime;
    }
    if (!metadata.texture_format.empty()) {
        j["texture_format"] = metadata.texture_format;
    }
    if (metadata.release_behavior != FrameSlotNativeImportReleaseBehavior::Unknown) {
        j["release_behavior"] = frame_slot_native_import_release_behavior_name(metadata.release_behavior);
    }
}

inline void from_json(const nlohmann::json& j, FrameSlotNativeImportMetadata& metadata) {
    metadata = FrameSlotNativeImportMetadata{};
    frame_slot_contract_detail::get_optional_alias(j, "runtime", "runtime", metadata.runtime);
    frame_slot_contract_detail::get_optional_alias(j, "texture_format", "textureFormat", metadata.texture_format);
    if (const auto it = frame_slot_contract_detail::find_optional_member(j, {"release_behavior", "releaseBehavior"});
        it != j.end()) {
        metadata.release_behavior = frame_slot_native_import_release_behavior_from_name(it->get<std::string>());
    }
}

inline void to_json(nlohmann::json& j, const LinuxImportableImageHandle& image) {
    j = nlohmann::json{
        {"kind", linux_importable_image_handle_kind_name(image.kind)},
        {"handle_type", image.handle_type},
        {"handle", image.handle},
    };
}

inline void from_json(const nlohmann::json& j, LinuxImportableImageHandle& image) {
    image = LinuxImportableImageHandle{};
    if (const auto it = frame_slot_contract_detail::find_optional_member(j, {"kind"}); it != j.end()) {
        image.kind = linux_importable_image_handle_kind_from_name(it->get<std::string>());
    }
    frame_slot_contract_detail::get_optional_alias(j, "handle_type", "handleType", image.handle_type);
    frame_slot_contract_detail::get_optional_alias(j, "handle", "handle", image.handle);
    if (image.kind == LinuxImportableImageHandleKind::Unknown && !image.handle_type.empty()) {
        image.kind = LinuxImportableImageHandleKind::OpaqueToken;
    }
}

inline void to_json(nlohmann::json& j, const LinuxImportableFrameSlotContract& contract) {
    j = nlohmann::json{
        {"image", contract.image},
        {"ready_sync", contract.ready_sync},
    };
    if (!contract.release_token.empty()) {
        j["release_token"] = contract.release_token;
    }
    if (contract.metadata.has_value()) {
        j["metadata"] = *contract.metadata;
    }
}

inline void from_json(const nlohmann::json& j, LinuxImportableFrameSlotContract& contract) {
    contract = LinuxImportableFrameSlotContract{};
    if (const auto it = frame_slot_contract_detail::find_optional_member(j, {"image"}); it != j.end()) {
        it->get_to(contract.image);
    }
    if (const auto it = frame_slot_contract_detail::find_optional_member(j, {"ready_sync", "readySync"});
        it != j.end()) {
        it->get_to(contract.ready_sync);
    }
    frame_slot_contract_detail::get_optional_alias(j, "release_token", "releaseToken", contract.release_token);
    if (const auto it = frame_slot_contract_detail::find_optional_member(
            j, {"metadata", "native_import", "nativeImport", "native_import_metadata", "nativeImportMetadata"});
        it != j.end()) {
        FrameSlotNativeImportMetadata metadata;
        it->get_to(metadata);
        contract.metadata = std::move(metadata);
    }
}

inline void to_json(nlohmann::json& j, const FrameSlotDescriptor& descriptor) {
    nlohmann::json frame_id_json = nlohmann::json::object();
    frame_slot_contract_detail::to_json_live_frame_id(frame_id_json, descriptor.frame_id);
    const std::optional<LinuxImportableFrameSlotContract> linux_import = frame_slot_linux_import_contract(descriptor);
    const FrameSlotReadySync& ready_sync = linux_import.has_value() ? linux_import->ready_sync : descriptor.ready_sync;
    j = nlohmann::json{
        {"slot_name", descriptor.slot_name},
        {"transport", frame_transport_name(descriptor.transport)},
        {"pixel_format", frame_pixel_format_name(descriptor.pixel_format)},
        {"slot_index", descriptor.slot_index},
        {"frame_id", std::move(frame_id_json)},
        {"capture_region", descriptor.capture_region},
        {"width", descriptor.width},
        {"height", descriptor.height},
        {"row_stride_bytes", descriptor.row_stride_bytes},
        {"byte_length", descriptor.byte_length},
        {"ready_ns", descriptor.ready_ns},
        {"short_frame", descriptor.short_frame},
        {"lifecycle", frame_slot_lifecycle_name(descriptor.lifecycle)},
        {"ownership", frame_slot_ownership_name(descriptor.ownership)},
        {"ready_sync", ready_sync},
    };
    if (linux_import.has_value()) {
        j["linux_import"] = *linux_import;
    }
}

inline void from_json(const nlohmann::json& j, FrameSlotDescriptor& descriptor) {
    descriptor = FrameSlotDescriptor{};
    frame_slot_contract_detail::get_optional_alias(j, "slot_name", "slotName", descriptor.slot_name);
    descriptor.transport = frame_transport_from_name(j.at("transport").get<std::string>());
    descriptor.pixel_format = frame_pixel_format_from_name(j.at("pixel_format").get<std::string>());
    frame_slot_contract_detail::get_optional_alias(j, "slot_index", "slotIndex", descriptor.slot_index);
    frame_slot_contract_detail::from_json_live_frame_id(j.at("frame_id"), descriptor.frame_id);
    j.at("capture_region").get_to(descriptor.capture_region);
    frame_slot_contract_detail::get_optional_alias(j, "width", "width", descriptor.width);
    frame_slot_contract_detail::get_optional_alias(j, "height", "height", descriptor.height);
    frame_slot_contract_detail::get_optional_alias(j, "row_stride_bytes", "rowStrideBytes",
                                                   descriptor.row_stride_bytes);
    frame_slot_contract_detail::get_optional_alias(j, "byte_length", "byteLength", descriptor.byte_length);
    frame_slot_contract_detail::get_optional_alias(j, "ready_ns", "readyNs", descriptor.ready_ns);
    frame_slot_contract_detail::get_optional_alias(j, "short_frame", "shortFrame", descriptor.short_frame);
    if (const auto it = frame_slot_contract_detail::find_optional_member(j, {"linux_import", "linuxImport"});
        it != j.end()) {
        LinuxImportableFrameSlotContract contract;
        it->get_to(contract);
        descriptor.linux_import = std::move(contract);
    }
    if (const auto it = frame_slot_contract_detail::find_optional_member(j, {"lifecycle"}); it != j.end()) {
        descriptor.lifecycle = frame_slot_lifecycle_from_name(it->get<std::string>());
    }
    if (const auto it = frame_slot_contract_detail::find_optional_member(j, {"ownership"}); it != j.end()) {
        descriptor.ownership = frame_slot_ownership_from_name(it->get<std::string>());
    }
    if (const auto it = frame_slot_contract_detail::find_optional_member(j, {"ready_sync", "readySync"});
        it != j.end()) {
        it->get_to(descriptor.ready_sync);
    }
    normalize_linux_importable_frame_slot_contract(descriptor);
}

}  
