

#include <catch2/internal/catch_errno_guard.hpp>

#include <cerrno>

namespace Catch {
ErrnoGuard::ErrnoGuard() : m_oldErrno(errno) {}
ErrnoGuard::~ErrnoGuard() {
    errno = m_oldErrno;
}
}  
