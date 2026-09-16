#pragma once
namespace mmltk::common::io {
// Owns a file descriptor and closes it on destruction. Moving transfers ownership and leaves the
// source empty; a negative value means "no descriptor" and is never closed.
class ScopedFd {
   public:
    ScopedFd() = default;
    explicit ScopedFd(int fd) noexcept;
    ~ScopedFd();
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept;
    ScopedFd& operator=(ScopedFd&& other) noexcept;
    [[nodiscard]] int get() const noexcept;
    [[nodiscard]] int release() noexcept;
    void reset(int next_fd = -1) noexcept;

   private:
    int fd_ = -1;
};
}  // namespace mmltk::common::io
