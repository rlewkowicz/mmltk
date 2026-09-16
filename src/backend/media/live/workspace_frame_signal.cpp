#include "detail/workspace_frame_signal.h"
namespace mmltk::backend::media::live {
void LiveCompletedFramePublication::publish(const PhysicalFrameRevision revision) noexcept {
    if (revision.valid() && revision.revision > revision_.load(std::memory_order_seq_cst)) store(revision);
}
PhysicalFrameRevision LiveCompletedFramePublication::snapshot() const noexcept {
    for (;;) {
        const std::uint64_t before = sequence_.load(std::memory_order_seq_cst);
        if ((before & 1U) != 0U) continue;
        const PhysicalFrameRevision result{revision_.load(std::memory_order_seq_cst),
                                           {frame_session_.load(std::memory_order_seq_cst), frame_sequence_.load(std::memory_order_seq_cst)},
                                           slot_.load(std::memory_order_seq_cst),
                                           ready_.load(std::memory_order_seq_cst)};
        if (sequence_.load(std::memory_order_seq_cst) == before) return result;
    }
}
void LiveCompletedFramePublication::clear() noexcept { store({}); }
void LiveCompletedFramePublication::store(const PhysicalFrameRevision revision) noexcept {
    const std::uint64_t before = sequence_.fetch_add(1U, std::memory_order_seq_cst);
    if ((before & 1U) != 0U) std::terminate();
    revision_.store(revision.revision, std::memory_order_seq_cst);
    frame_session_.store(revision.frame.session, std::memory_order_seq_cst);
    frame_sequence_.store(revision.frame.sequence, std::memory_order_seq_cst);
    slot_.store(revision.slot, std::memory_order_seq_cst);
    ready_.store(revision.ready_event, std::memory_order_seq_cst);
    sequence_.store(before + 2U, std::memory_order_seq_cst);
}
}  // namespace mmltk::backend::media::live
