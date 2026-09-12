#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

// Canonical frozen import ABI. The native generator projects data-only Rust
// records, integer wire fields, enum values, and every size/alignment/offset
// assertion into the build-owned graphics artifact consumed by Firefox.
// kAbiVersion changes only when layout or opcode meaning changes.
namespace mmltk::controller::presentation::detail::workspace_surface_import {

inline constexpr std::uint32_t kAbiVersion = 11U;

// The underlying type is the wire type: the Rust half of this record declares
// `u32` fields, and the static assertions below are what keep the two frozen
// layouts identical, so this may not shrink to fit its value set.
// NOLINTNEXTLINE(performance-enum-size)
enum class Opcode : std::uint32_t {
    // Host to shell, carrying three descriptors: the allocation described here is
    // admitted under `id`. The descriptors are ordered by
    // `kImportMemoryDescriptor`, `kImportFrameEdgeDescriptor`, and
    // `kImportFrameSignalDescriptor` below.
    Import = 1U,
    // Host to shell: `id` is withdrawn. A live capability completes through the
    // exact Retired terminal after its source read or arena page sampling ends.
    Drop = 2U,
    // Shell to host: source `id` is imported and initialized. Carries
    // one exported Vulkan timeline semaphore descriptor.
    Ready = 3U,
    // Shell to host: `id` produced no texture, with a reason in `code`.
    Failed = 4U,
    // Shell to host: a capacity-exhausted private mailbox layer regained the
    // exact writable slot. `code - 1` packs `layer * 2 + slot`; `stride` and
    // `size` carry the opaque logical content identity that completed, and
    // `presentation_revision` carries the exact arena sample revision.
    Available = 5U,
    // Shell to host: the exact mailbox sample identified by `code`, `stride`,
    // `size`, and `presentation_revision` was occupied and offered to the page.
    Presented = 6U,
    // Shell to host: the page released that exact sample after rejection or
    // final GPU use.
    Completed = 7U,
    // Shell to host: the live peer has released every page sample and destroyed
    // the private mirror/resource state for this withdrawn capability.
    Retired = 8U,
    // A page-visible, reusable two-slot destination; carries no source memory.
    Arena = 9U,
    // Arena creation and its exact device/layout negotiation have completed.
    ArenaReady = 10U,
    // Host observed the source's matching even timeline value. Independent of
    // queue-ordered frame delivery and of the page's final sample release.
    ReadSettled = 11U,
    // Shell won the physical generation gate for this exact completed offer.
    Acquired = 12U,
};

// Why the shell produced no texture. These describe what happened in the shell,
// not what the host should do about it.
// The underlying type is the wire type: the Rust half of this record declares
// `u32` fields, and the static assertions below are what keep the two frozen
// layouts identical, so this may not shrink to fit its value set.
// NOLINTNEXTLINE(performance-enum-size)
enum class FailureCode : std::uint32_t {
    NotAdmitted = 1U,
    UnsupportedDescriptor = 2U,
    Import = 3U,
    // The descriptor was importable but its row pitch or size did not match what
    // the browser's device requires for a linear image of that extent. The
    // record's `stride` and `size` carry the layout that would be accepted, and
    // this is the only reply that carries a layout: the required pitch is a
    // property of the importing driver that the exporting side cannot compute.
    Layout = 4U,
};

// The only modifier either side imports. The host exports a linear CUDA range,
// and a linear-tiled image is the layout both sides agree on without a modifier
// negotiation neither side needs.
inline constexpr std::uint64_t kModifierLinear = 0U;

// Descriptor positions for the two record kinds that carry SCM_RIGHTS:
// `Import` carries memory, its edge, and a frame-identity descriptor that the
// shell accesses with atomic loads only;
// `Ready` carries the timeline.
//
// The frame edge is an eventfd the host signals once per completed arena mirror
// and never reads or waits on. The shell watches it and blits the admitted
// allocation into the texture the page samples; an eventfd read returns the
// number of signals since the last read, so a backlog coalesces into one blit.
inline constexpr std::size_t kImportMemoryDescriptor = 0U;
inline constexpr std::size_t kImportFrameEdgeDescriptor = 1U;
inline constexpr std::size_t kImportFrameSignalDescriptor = 2U;
inline constexpr std::size_t kImportAccessDescriptor = 3U;
inline constexpr std::uint32_t kImportDescriptorCount = 4U;
inline constexpr std::size_t kReadyTimelineDescriptor = 0U;
inline constexpr std::uint32_t kReadyDescriptorCount = 1U;

struct Record {
    std::uint32_t abi_version = kAbiVersion;
    Opcode opcode = Opcode::Import;
    std::uint64_t id_high = 0U;
    std::uint64_t id_low = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint64_t stride = 0U;
    std::uint64_t size = 0U;
    std::uint64_t modifier = kModifierLinear;
    std::uint32_t code = 0U;
    // How many descriptors accompany this record. The receiving side rejects a
    // record whose ancillary data does not carry exactly this many, which is
    // what keeps the two independent declarations of this ABI honest about the
    // one message that carries more than a payload.
    std::uint32_t descriptors = 0U;
    // Exact generation-scoped arena publication copied into a private mailbox
    // slot. Meaningful only for Available, Presented, and Completed.
    std::uint64_t presentation_revision = 0U;
    std::uint64_t arena_high = 0U;
    std::uint64_t arena_low = 0U;
    std::uint64_t allocation_identity = 0U;
    std::uint64_t device_incarnation = 0U;
    // Import/ArenaReady: image byte offset. ReadSettled: exact source transfer sequence.
    std::uint64_t offset = 0U;
    std::uint64_t alignment = 0U;
    std::uint8_t device_uuid[16]{};
    std::uint32_t dedicated = 0U;
    std::uint32_t memory_type_bits = 0U;
    // Exact negotiated external-image usage: zero uses the capability copy
    // route; one permits sampling the producer image in GENERAL layout.
    std::uint64_t direct_sampling = 0U;
};

static_assert(std::is_standard_layout_v<Record>);
static_assert(std::is_trivially_copyable_v<Record>);
static_assert(alignof(Record) == 8U);
static_assert(sizeof(Record) == 152U);
static_assert(offsetof(Record, abi_version) == 0U);
static_assert(offsetof(Record, opcode) == 4U);
static_assert(offsetof(Record, id_high) == 8U);
static_assert(offsetof(Record, id_low) == 16U);
static_assert(offsetof(Record, width) == 24U);
static_assert(offsetof(Record, height) == 28U);
static_assert(offsetof(Record, stride) == 32U);
static_assert(offsetof(Record, size) == 40U);
static_assert(offsetof(Record, modifier) == 48U);
static_assert(offsetof(Record, code) == 56U);
static_assert(offsetof(Record, descriptors) == 60U);
static_assert(offsetof(Record, presentation_revision) == 64U);
static_assert(kImportMemoryDescriptor < kImportDescriptorCount);
static_assert(kImportFrameEdgeDescriptor < kImportDescriptorCount);
static_assert(kImportFrameSignalDescriptor < kImportDescriptorCount);
static_assert(kImportMemoryDescriptor != kImportFrameEdgeDescriptor);
static_assert(kImportMemoryDescriptor != kImportFrameSignalDescriptor);
static_assert(kImportFrameEdgeDescriptor != kImportFrameSignalDescriptor);
static_assert(kReadyTimelineDescriptor < kReadyDescriptorCount);

// A plain packet lets either native boundary validate the complete normative
// layout without importing the other language's declaration.
struct LayoutPacket {
    std::uint32_t abi_version = kAbiVersion;
    std::uint32_t record_size = sizeof(Record);
    std::uint32_t record_alignment = alignof(Record);
    std::uint32_t import_descriptor_count = kImportDescriptorCount;
    std::uint32_t ready_descriptor_count = kReadyDescriptorCount;
    std::uint32_t opcode_offset = offsetof(Record, opcode);
    std::uint32_t capability_offset = offsetof(Record, id_high);
    std::uint32_t presentation_revision_offset = offsetof(Record, presentation_revision);
};

static_assert(std::is_standard_layout_v<LayoutPacket>);
static_assert(std::is_trivially_copyable_v<LayoutPacket>);

[[nodiscard]] inline constexpr LayoutPacket layout_packet() noexcept { return {}; }

[[nodiscard]] inline constexpr bool valid(const LayoutPacket& packet) noexcept {
    const LayoutPacket expected{};
    return packet.abi_version == expected.abi_version && packet.record_size == expected.record_size &&
           packet.record_alignment == expected.record_alignment && packet.import_descriptor_count == expected.import_descriptor_count &&
           packet.ready_descriptor_count == expected.ready_descriptor_count && packet.opcode_offset == expected.opcode_offset &&
           packet.capability_offset == expected.capability_offset &&
           packet.presentation_revision_offset == expected.presentation_revision_offset;
}

// How many descriptors an opcode carries. A record that arrives with a
// different count is a framing violation on either side.
[[nodiscard]] inline constexpr std::uint32_t descriptor_count(const Opcode opcode) noexcept {
    if (opcode == Opcode::Import) { return kImportDescriptorCount; }
    return opcode == Opcode::Ready ? kReadyDescriptorCount : 0U;
}

[[nodiscard]] inline bool valid(const Record& record) noexcept {
    const bool known = record.opcode == Opcode::Import || record.opcode == Opcode::Drop || record.opcode == Opcode::Ready ||
                       record.opcode == Opcode::Failed || record.opcode == Opcode::Available || record.opcode == Opcode::Presented ||
                       record.opcode == Opcode::Completed || record.opcode == Opcode::Retired || record.opcode == Opcode::Arena ||
                       record.opcode == Opcode::ArenaReady || record.opcode == Opcode::ReadSettled || record.opcode == Opcode::Acquired;
    if (!known || record.abi_version != kAbiVersion || record.modifier != kModifierLinear ||
        record.descriptors != descriptor_count(record.opcode) || (record.id_high == 0U && record.id_low == 0U)) {
        return false;
    }
    const bool empty_extent = record.width == 0U && record.height == 0U && record.stride == 0U && record.size == 0U;
    bool uuid_valid = false;
    for (const auto byte : record.device_uuid)
        uuid_valid = uuid_valid || byte != 0U;
    const bool empty_layout = record.arena_high == 0U && record.arena_low == 0U && record.allocation_identity == 0U &&
                              record.device_incarnation == 0U &&
                              (record.offset == 0U || record.opcode == Opcode::ReadSettled || record.opcode == Opcode::Acquired) &&
                              record.alignment == 0U && !uuid_valid && record.dedicated == 0U && record.memory_type_bits == 0U &&
                              record.direct_sampling == 0U;
    if (record.opcode != Opcode::Import && record.opcode != Opcode::ArenaReady && !empty_layout) return false;
    const bool layout_valid = record.width != 0U && record.height != 0U && record.stride >= static_cast<std::uint64_t>(record.width) * 4U &&
                              record.offset <= record.size && record.stride != 0U &&
                              record.height <= (record.size - record.offset) / record.stride && record.alignment != 0U &&
                              (record.alignment & (record.alignment - 1U)) == 0U &&
                              record.size <= static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max()) &&
                              record.device_incarnation != 0U && uuid_valid && record.dedicated <= 1U && record.memory_type_bits != 0U &&
                              record.direct_sampling <= 1U;
    switch (record.opcode) {
        case Opcode::Import: {
            return layout_valid && record.allocation_identity != 0U && (record.arena_high != 0U || record.arena_low != 0U) &&
                   record.code == 0U && record.modifier == kModifierLinear && record.presentation_revision == 0U;
        }
        case Opcode::Arena:
            return record.width != 0U && record.height != 0U && record.stride == 0U && record.size == 0U && record.code == 0U &&
                   record.presentation_revision == 0U;
        case Opcode::ArenaReady:
            return layout_valid && record.arena_high == 0U && record.arena_low == 0U && record.allocation_identity == 0U &&
                   record.modifier == kModifierLinear && record.code == 0U && record.presentation_revision == 0U;
        case Opcode::ReadSettled:
        case Opcode::Acquired:
            return record.offset != 0U && record.width == 0U && record.height == 0U && record.code == 0U &&
                   (record.stride != 0U || record.size != 0U) && record.presentation_revision != 0U;
        case Opcode::Drop:
        case Opcode::Ready:
        case Opcode::Retired:
            return empty_extent && record.modifier == kModifierLinear && record.code == 0U && record.presentation_revision == 0U;
        case Opcode::Available:
        case Opcode::Presented:
        case Opcode::Completed:
            return record.width == 0U && record.height == 0U && record.modifier == kModifierLinear && record.code >= 1U &&
                   record.code <= 2U && (record.stride != 0U || record.size != 0U) && record.presentation_revision != 0U;
        case Opcode::Failed: {
            const auto failure = static_cast<FailureCode>(record.code);
            const bool known_failure = failure == FailureCode::NotAdmitted || failure == FailureCode::UnsupportedDescriptor ||
                                       failure == FailureCode::Import || failure == FailureCode::Layout;
            const bool layout = failure == FailureCode::Layout;
            return known_failure && record.width == 0U && record.height == 0U && record.modifier == kModifierLinear &&
                   record.presentation_revision == 0U &&
                   (layout ? record.stride != 0U && record.size != 0U : record.stride == 0U && record.size == 0U);
        }
    }
    return false;
}

}  // namespace mmltk::controller::presentation::detail::workspace_surface_import
