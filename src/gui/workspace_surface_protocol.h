#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mmltk::gui::workspace_surface_protocol {

inline constexpr std::uint32_t kMagic = 0x574c4d4dU;
inline constexpr std::uint16_t kVersion = 3U;
inline constexpr std::size_t kTokenBytes = 64U;
inline constexpr std::size_t kDeviceUuidBytes = 16U;
inline constexpr std::uint32_t kFlagTimelineSemaphore = 1U << 0U;
inline constexpr std::uint32_t kFlagSlotReserved = 1U << 1U;

// NOLINTNEXTLINE(performance-enum-size)
enum class MessageType : std::uint16_t {
    Hello = 1,
    Adapter = 2,
    ConfigureSlot = 3,
    SlotReady = 4,
    Present = 5,
    Retire = 6,
    Ack = 7,
    Error = 8,
    ReserveSlot = 9,
};

struct Message {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    MessageType type = MessageType::Error;
    std::uint32_t size = sizeof(Message);
    std::uint32_t flags = 0U;
    std::uint32_t slot = 0U;
    std::uint32_t slot_count = 0U;
    std::uint32_t modifier_index = 0U;
    std::uint32_t modifier_count = 0U;
    std::uint64_t generation = 0U;
    std::uint64_t revision = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t source_x = 0U;
    std::uint32_t source_y = 0U;
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::uint32_t render_major = 0U;
    std::uint32_t render_minor = 0U;
    std::uint64_t stride = 0U;
    std::uint64_t offset = 0U;
    std::uint64_t allocation_size = 0U;
    std::uint64_t drm_modifier = 0U;
    std::uint32_t drm_format = 0U;
    std::uint32_t error_code = 0U;
    std::uint64_t capture_ns = 0U;
    std::uint64_t ready_ns = 0U;
    std::array<std::uint8_t, kDeviceUuidBytes> device_uuid{};
    std::array<char, kTokenBytes> token{};
    std::array<std::uint8_t, 8U> reserved{};
};

static_assert(sizeof(Message) == 224U);

[[nodiscard]] inline bool valid_header(const Message& message) noexcept {
    return message.magic == kMagic && message.version == kVersion && message.size == sizeof(Message);
}

}  
