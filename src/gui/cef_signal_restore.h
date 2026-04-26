#pragma once

namespace mmltk::gui {

void backup_posix_signal_handlers() noexcept;
void restore_posix_signal_handlers() noexcept;

}  // namespace mmltk::gui
