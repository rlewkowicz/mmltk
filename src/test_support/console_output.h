#pragma once
#include <unistd.h>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
namespace mmltk::testsupport::console_output {
inline void trim_output_tail(std::string& output_tail, std::size_t max_size = 65536) {
 if (output_tail.size() > max_size) { output_tail.erase(0, output_tail.size() - max_size); }
}
inline void append_console_output(std::string& tail, const std::string_view chunk, std::size_t max_size = 65536) {
 std::size_t index = 0;
 while (index < chunk.size()) {
  const char ch = chunk[index];
  if (ch == '\r') {
   const std::size_t newline = tail.rfind('\n');
   if (newline == std::string::npos) {
    tail.clear();
   } else {
    tail.erase(newline + 1);
   }
   ++index;
   continue;
  }
  if (ch == '\b') {
   if (!tail.empty()) { tail.pop_back(); }
   ++index;
   continue;
  }
  if (ch == '\033') {
   ++index;
   if (index < chunk.size() && chunk[index] == '[') {
    ++index;
    while (index < chunk.size()) {
     const auto code = static_cast<unsigned char>(chunk[index++]);
     if (code >= 0x40 && code <= 0x7E) { break; }
    }
   }
   continue;
  }
  tail.push_back(ch);
  ++index;
 }
 trim_output_tail(tail, max_size);
}
inline std::string read_fd(const int fd, const std::string_view error_prefix, const bool nonblocking = false) {
 std::string output;
 if (fd < 0) { return output; }
 std::array<char, 4096> buffer{};
 while (true) {
  const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
  if (bytes_read > 0) {
   output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
   continue;
  }
  if (bytes_read == 0) { break; }
  if (errno == EINTR) { continue; }
  if (nonblocking && (errno == EAGAIN || errno == EWOULDBLOCK)) { break; }
  throw std::runtime_error(std::string(error_prefix) + std::strerror(errno));
 }
 return output;
}
class ScopedStderrCapture final {
public:
 ScopedStderrCapture() {
  std::fflush(stderr);
  if (::pipe(pipe_fds_.data()) != 0) { throw std::runtime_error("pipe failed: " + std::string{std::strerror(errno)}); }
  saved_stderr_ = ::dup(STDERR_FILENO);
  if (saved_stderr_ < 0) {
   close_pipe();
   throw std::runtime_error("stderr capture failed: " + std::string{std::strerror(errno)});
  }
  if (::dup2(pipe_fds_[1], STDERR_FILENO) < 0) {
   static_cast<void>(::close(saved_stderr_));
   saved_stderr_ = -1;
   close_pipe();
   throw std::runtime_error("stderr capture failed: " + std::string{std::strerror(errno)});
  }
  static_cast<void>(::close(pipe_fds_[1]));
  pipe_fds_[1] = -1;
 }
 ~ScopedStderrCapture() noexcept {
  if (finished_) return;
  try {
   static_cast<void>(finish());
  } catch (...) {
   restore_stderr();
   close_pipe();
  }
 }
 ScopedStderrCapture(const ScopedStderrCapture&) = delete;
 ScopedStderrCapture& operator=(const ScopedStderrCapture&) = delete;
 std::string finish() {
  if (finished_) return output_;
  std::fflush(stderr);
  restore_stderr();
  output_ = mmltk::testsupport::console_output::read_fd(pipe_fds_[0], "read failed: ");
  static_cast<void>(::close(pipe_fds_[0]));
  pipe_fds_[0] = -1;
  finished_ = true;
  return output_;
 }

private:
 std::array<int, 2U> pipe_fds_{-1, -1};
 int saved_stderr_ = -1;
 bool finished_ = false;
 std::string output_;
 void restore_stderr() noexcept {
  if (saved_stderr_ < 0) return;
  static_cast<void>(::dup2(saved_stderr_, STDERR_FILENO));
  static_cast<void>(::close(saved_stderr_));
  saved_stderr_ = -1;
 }
 void close_pipe() noexcept {
  for (int& descriptor : pipe_fds_) {
   if (descriptor < 0) continue;
   static_cast<void>(::close(descriptor));
   descriptor = -1;
  }
 }
};
}  // namespace mmltk::testsupport::console_output
