#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>

#include "src/controller/presentation/detail/workspace_surface_import_channel.h"

namespace mmltk::controller::presentation::detail {

// CLEANUP-IGNORE: This cache-line signal is a fixed shared-memory protocol, not an application layout abstraction.
struct alignas(64) WorkspaceFrameSignal final {
    // CLEANUP-IGNORE: Signal timeline fields have atomic ABI offsets unrelated to image-layout dimension sequences.
    std::uint64_t sequence_lock = 0U;
    std::uint64_t timeline_ready = 0U;
    std::uint64_t transfer_sequence = 0U;
    std::uint64_t layer = 0U;
    std::uint64_t content_session = 0U;
    std::uint64_t content_sequence = 0U;
    std::uint64_t presentation_revision = 0U;
    std::uint32_t content_width = 0U;
    std::uint32_t content_height = 0U;
};

static_assert(std::is_standard_layout_v<WorkspaceFrameSignal>);
static_assert(std::is_trivially_copyable_v<WorkspaceFrameSignal>);
static_assert(sizeof(WorkspaceFrameSignal) == 64U);
static_assert(alignof(WorkspaceFrameSignal) == 64U);
static_assert(offsetof(WorkspaceFrameSignal, sequence_lock) == 0U);
static_assert(offsetof(WorkspaceFrameSignal, timeline_ready) == 8U);
static_assert(offsetof(WorkspaceFrameSignal, transfer_sequence) == 16U);
static_assert(offsetof(WorkspaceFrameSignal, layer) == 24U);
static_assert(offsetof(WorkspaceFrameSignal, content_session) == 32U);
static_assert(offsetof(WorkspaceFrameSignal, content_sequence) == 40U);
static_assert(offsetof(WorkspaceFrameSignal, presentation_revision) == 48U);
static_assert(offsetof(WorkspaceFrameSignal, content_width) == 56U);
static_assert(offsetof(WorkspaceFrameSignal, content_height) == 60U);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);

[[nodiscard]] inline constexpr std::uint64_t workspace_timeline_ready(const std::uint64_t transfer_sequence) {
    constexpr std::uint64_t kMaximumTransferSequence = std::numeric_limits<std::uint64_t>::max() / 2U + 1U;
    if (transfer_sequence == 0U || transfer_sequence > kMaximumTransferSequence)
        throw std::overflow_error("workspace transfer sequence is out of range");
    return (transfer_sequence - 1U) * 2U + 1U;
}

inline void publish_workspace_frame_signal(WorkspaceFrameSignal* const signal, const std::uint64_t timeline_ready,
                                           const std::uint64_t transfer_sequence, const WorkspacePresentationLayer layer,
                                           const WorkspaceContentIdentity logical_content, const std::uint64_t presentation_revision,
                                           const std::uint32_t content_width, const std::uint32_t content_height) noexcept {
    if (signal == nullptr) return;
    std::atomic_ref<std::uint64_t> sequence{signal->sequence_lock};
    static_cast<void>(sequence.fetch_add(1U, std::memory_order_seq_cst));
    std::atomic_ref<std::uint64_t>{signal->timeline_ready}.store(timeline_ready, std::memory_order_seq_cst);
    std::atomic_ref<std::uint64_t>{signal->transfer_sequence}.store(transfer_sequence, std::memory_order_seq_cst);
    std::atomic_ref<std::uint64_t>{signal->layer}.store(static_cast<std::uint64_t>(layer), std::memory_order_seq_cst);
    std::atomic_ref<std::uint64_t>{signal->content_session}.store(logical_content.session, std::memory_order_seq_cst);
    std::atomic_ref<std::uint64_t>{signal->content_sequence}.store(logical_content.sequence, std::memory_order_seq_cst);
    std::atomic_ref<std::uint64_t>{signal->presentation_revision}.store(presentation_revision, std::memory_order_seq_cst);
    std::atomic_ref<std::uint32_t>{signal->content_width}.store(content_width, std::memory_order_seq_cst);
    std::atomic_ref<std::uint32_t>{signal->content_height}.store(content_height, std::memory_order_seq_cst);
    static_cast<void>(sequence.fetch_add(1U, std::memory_order_seq_cst));
}

}  // namespace mmltk::controller::presentation::detail
