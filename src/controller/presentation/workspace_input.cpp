#include "src/controller/presentation/workspace_input.h"

namespace mmltk::controller {

void WorkspaceInput::SetPeer(std::uint64_t epoch) noexcept {
    peer_ = epoch;
    buttons_ = 0U;
    latest_.reset();
    queue_.Clear();
}

void WorkspaceInput::Accept(WorkspaceMouse mouse, PresentationSourceKind source) {
    if (!mouse.valid() || mouse.source != source || mouse.peer_epoch != peer_)
        throw contracts::InvalidIntentError("Workspace mouse ownership is invalid");
    queue_.Push(std::move(mouse));
    while (auto record = queue_.Pop()) {
        const auto bit = static_cast<std::uint8_t>(1U << static_cast<unsigned>(record->button));
        if (record->kind == WorkspaceMouseKind::Press) buttons_ |= bit;
        if (record->kind == WorkspaceMouseKind::Release) buttons_ &= static_cast<std::uint8_t>(~bit);
        if (record->kind == WorkspaceMouseKind::Cancel) buttons_ = 0U;
        latest_ = std::move(*record);
    }
}

bool WorkspaceInput::pressed(WorkspaceMouseButton button) const noexcept {
    return (buttons_ & (1U << static_cast<unsigned>(button))) != 0U;
}

}  // namespace mmltk::controller
