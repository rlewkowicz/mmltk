#pragma once
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <mutex>
#include <netinet/in.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <utility>
#include <vector>
#include "src/common/io/scoped_fd.h"
#include "src/test_support/async_test_utils.hpp"
namespace mmltk::backend::data::testsupport {
class HttpServer {
public:
 // The payload is borrowed until Stop(); retain it and mutate only between settled transfers.
 explicit HttpServer(std::span<const std::uint8_t> payload) : HttpServer(payload, payload.size()) {}
 explicit HttpServer(const std::string& payload) : HttpServer(std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()}) {}
 HttpServer(std::string&&) = delete;
 HttpServer(std::vector<std::uint8_t>&&) = delete;
 explicit HttpServer(std::size_t generated_bytes) : HttpServer({}, generated_bytes) {}
 HttpServer(std::span<const std::uint8_t> payload, std::size_t bytes) : payload_(payload), payload_size_(bytes), listener_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
  require_condition(listener_.get() >= 0, "failed to create benchmark HTTP socket");
  const int reuse = 1;
  require_condition(::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0, "failed to configure benchmark HTTP socket");
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  require_condition(::bind(listener_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "failed to bind benchmark HTTP socket");
  require_condition(::listen(listener_.get(), 8) == 0, "failed to listen on benchmark HTTP socket");
  socklen_t length = sizeof(address);
  require_condition(::getsockname(listener_.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0, "failed to inspect benchmark HTTP socket");
  port_ = ntohs(address.sin_port);
  worker_ = std::jthread([this] {
   try {
    run();
   } catch (...) { failure_ = std::current_exception(); }
  });
 }
 HttpServer(const HttpServer&) = delete;
 HttpServer& operator=(const HttpServer&) = delete;
 ~HttpServer() { Stop(); }
 void Stop() noexcept {
  stop_.store(true, std::memory_order_release);
  partial_.Release();
  ::shutdown(listener_.get(), SHUT_RDWR);
  {
   std::scoped_lock lock(client_mutex_);
   if (active_client_ >= 0) ::shutdown(active_client_, SHUT_RDWR);
  }
  if (worker_.joinable()) worker_.join();
 }
 void Check() {
  Stop();
  if (failure_) std::rethrow_exception(failure_);
 }
 static constexpr std::size_t partial_bytes = 512U * 1024U;
 void GateNextTransfer() { gate_next_.store(true, std::memory_order_release); }
 [[nodiscard]] bool WaitPartial() const { return partial_.WaitEntered(std::chrono::seconds{3}); }
 void ReleasePartial() const { partial_.Release(); }
 [[nodiscard]] std::string url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port_) + "/" + path; }
 void RestartNextRangedTransfer() { restart_range_.store(true, std::memory_order_release); }
 void TruncateNextTransfer() { truncate_next_.store(true, std::memory_order_release); }
 [[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>> ranges() {
  const std::scoped_lock lock(client_mutex_);
  return ranges_;
 }
 void fail_next(const int count, const std::size_t body_bytes = 0U) {
  require_condition(body_bytes <= 64U * 1024U, "HTTP failure fixture body exceeds bound");
  failure_body_bytes_.store(body_bytes, std::memory_order_relaxed);
  failures_remaining_.store(count, std::memory_order_release);
 }
 void RedirectNextTransfer() { redirect_next_.store(true, std::memory_order_release); }
 void OmitContentLength() { omit_length_.store(true, std::memory_order_release); }
 [[nodiscard]] std::uint64_t requests() const { return requests_.load(std::memory_order_relaxed); }
 [[nodiscard]] std::uint64_t ranged_requests() const { return ranged_requests_.load(std::memory_order_relaxed); }

private:
 static void require_condition(const bool condition, const char* message) {
  if (!condition) { throw std::runtime_error(message); }
 }
 static bool send_all(const int socket, const void* data, const std::size_t bytes) {
  const auto* input = static_cast<const std::uint8_t*>(data);
  std::size_t sent = 0;
  while (sent < bytes) {
   const ssize_t result = ::send(socket, input + sent, bytes - sent, MSG_NOSIGNAL);
   if (result < 0 && errno == EINTR) continue;
   if (result <= 0) { return false; }
   sent += static_cast<std::size_t>(result);
  }
  return true;
 }
 static void send_discarded_body(const int client, const std::string_view status, const std::size_t bytes, const std::string_view extra_headers = {}) {
  const std::string header = "HTTP/1.1 " + std::string(status) + "\r\nContent-Length: " + std::to_string(bytes) + "\r\n" + std::string(extra_headers) + "Connection: close\r\n\r\n";
  const std::string body(bytes, '!');
  if (send_all(client, header.data(), header.size())) { (void)send_all(client, body.data(), body.size()); }
 }
 void run() {
  while (!stop_.load(std::memory_order_relaxed)) {
   const mmltk::common::io::ScopedFd client_owner{::accept4(listener_.get(), nullptr, nullptr, SOCK_CLOEXEC)};
   const int client = client_owner.get();
   if (client < 0) {
    if (stop_.load(std::memory_order_relaxed)) { return; }
    if (errno == EINTR) continue;
    throw std::runtime_error("HTTP fixture accept failed");
   }
   {
    std::scoped_lock lock(client_mutex_);
    if (stop_.load(std::memory_order_acquire)) return;
    active_client_ = client;
   }
   const mmltk::testsupport::ScopedTestCleanup clear_client([&] {
    std::scoped_lock lock(client_mutex_);
    active_client_ = -1;
   });
   const timeval deadline{.tv_sec = 3, .tv_usec = 0};
   require_condition(::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &deadline, sizeof(deadline)) == 0, "HTTP receive deadline");
   require_condition(::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline)) == 0, "HTTP send deadline");
   serve(client);
  }
 }
 void serve(const int client) {
  std::size_t received = 0U;
  std::string_view request;
  while (received < receive_.size()) {
   const auto count = ::recv(client, receive_.data() + received, receive_.size() - received, 0);
   if (count < 0 && errno == EINTR) continue;
   if (count <= 0) return;
   received += static_cast<std::size_t>(count);
   request = {receive_.data(), received};
   if (request.find("\r\n\r\n") != std::string_view::npos) break;
  }
  require_condition(request.find("\r\n\r\n") != std::string_view::npos, "HTTP header exceeds bounded receive storage");
  requests_.fetch_add(1U, std::memory_order_relaxed);
  if (failures_remaining_.fetch_sub(1, std::memory_order_acquire) > 0) {
   send_discarded_body(client, "503 Service Unavailable", failure_body_bytes_.load(std::memory_order_relaxed));
   return;
  }
  failures_remaining_.store(0, std::memory_order_relaxed);
  if (redirect_next_.exchange(false, std::memory_order_acq_rel)) {
   send_discarded_body(client, "302 Found", 8192U, "Location: /redirected\r\n");
   return;
  }
  std::size_t begin = 0U;
  std::size_t end = payload_size_ - 1U;
  bool ranged = false;
  const std::size_t range_header = request.find("\r\nRange: bytes=");
  if (range_header != std::string::npos) {
   const std::size_t number_begin = range_header + std::strlen("\r\nRange: bytes=");
   const std::size_t dash = request.find('-', number_begin);
   if (dash != std::string::npos) {
    const auto parsed = std::from_chars(request.data() + number_begin, request.data() + dash, begin);
    require_condition(parsed.ec == std::errc{} && parsed.ptr == request.data() + dash, "invalid HTTP range");
    const auto range_end = request.find("\r\n", dash);
    if (range_end != dash + 1U) {
     const auto last = std::from_chars(request.data() + dash + 1U, request.data() + range_end, end);
     require_condition(last.ec == std::errc{} && last.ptr == request.data() + range_end, "invalid HTTP range end");
    }
    ranged = begin <= end && end < payload_size_;
   }
  }
  if (ranged) {
   ranged_requests_.fetch_add(1U, std::memory_order_relaxed);
   const std::scoped_lock lock(client_mutex_);
   ranges_.emplace_back(begin, end);
  } else {
   begin = 0U;
  }
  const bool identity_probe = ranged && begin == 0U && end == 0U;
  if (ranged && !identity_probe && !request.starts_with("HEAD ") && restart_range_.exchange(false, std::memory_order_acq_rel)) {
   ranged = false;
   begin = 0U;
   end = payload_size_ - 1U;
  }
  const std::size_t bytes = end + 1U - begin;
  std::string header = ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
  if (!omit_length_.load(std::memory_order_acquire)) { header += "Content-Length: " + std::to_string(bytes) + "\r\n"; }
  header +=
   "Accept-Ranges: bytes\r\nETag: \"benchmark-test-etag\"\r\n"
   "Last-Modified: Thu, 23 Jul 2026 12:00:00 GMT\r\n";
  if (ranged) { header += "Content-Range: bytes " + std::to_string(begin) + "-" + std::to_string(end) + "/" + std::to_string(payload_size_) + "\r\n"; }
  header += "Connection: close\r\n\r\n";
  if (!send_all(client, header.data(), header.size())) { return; }
  constexpr std::size_t chunk = std::size_t{16U} * 1024U;
  if (request.starts_with("HEAD ")) return;
  const bool gated = bytes >= partial_bytes && gate_next_.exchange(false, std::memory_order_acq_rel);
  const bool truncated = bytes >= partial_bytes && truncate_next_.exchange(false, std::memory_order_acq_rel);
  std::array<std::uint8_t, chunk> generated{};
  std::size_t offset = begin;
  while (offset <= end) {
   const std::size_t current = std::min(chunk, end + 1U - offset);
   const auto* data = payload_.data();
   if (payload_.empty()) {
    for (std::size_t i = 0; i < current; ++i) generated[i] = static_cast<std::uint8_t>(((offset + i) * 131U + 17U) & 0xFFU);
    data = generated.data();
   } else {
    data += offset;
   }
   if (!send_all(client, data, current)) { return; }
   offset += current;
   if (truncated && offset - begin >= partial_bytes) return;
   if (gated && offset - begin == partial_bytes) partial_.receipt().ArriveAndWait();
   if (stop_.load(std::memory_order_acquire)) return;
  }
 }
 const std::span<const std::uint8_t> payload_;
 const std::size_t payload_size_;
 std::vector<std::pair<std::size_t, std::size_t>> ranges_;
 mmltk::common::io::ScopedFd listener_;
 std::array<char, 16U * 1024U> receive_{};
 std::mutex client_mutex_;
 int active_client_ = -1;
 std::exception_ptr failure_;
 mmltk::testsupport::TestGate partial_{"HTTP partial transfer byte boundary"};
 std::uint16_t port_ = 0U;
 std::atomic<bool> stop_{false};
 std::atomic<bool> gate_next_{false};
 std::atomic<bool> truncate_next_{false};
 std::atomic<bool> restart_range_{false};
 std::atomic<int> failures_remaining_{0};
 std::atomic<std::size_t> failure_body_bytes_{0U};
 std::atomic<bool> redirect_next_{false};
 std::atomic<bool> omit_length_{false};
 std::atomic<std::uint64_t> requests_{0U};
 std::atomic<std::uint64_t> ranged_requests_{0U};
 std::jthread worker_;
};
}  // namespace mmltk::backend::data::testsupport
