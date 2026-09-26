#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "src/backend/imaging/raster/rendered_image_writer.h"
#include "src/backend/imaging/raster/detail/checked_png.h"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/tests/device_execution_fixture.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/async_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cuda_runtime.h>
#include <stb_image.h>
#include <stb_image_write.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <stdexcept>
#include <sys/types.h>
#include <cstdint>
#include <system_error>
#include <vector>
#include <fstream>
#include <iterator>
#include <limits>
#include <utility>
namespace fs = std::filesystem;
namespace {
namespace gpu = mmltk::frameworks::gpu;
struct RenderedImageFixture final {
 gpu::test_support::IsolatedTestDevice device;
 gpu::SystemImageRuntime runtime{{.device = 0, .numa_node = device.execution.placement.numa_node, .execution = device.execution, .adopted_context = device.context}};
 void Fill(int value) {
  auto candidate = runtime.AcquireOutput();
  runtime.PublishRetained(candidate, 7U, 5U, [&](auto clean, auto, auto stream) {
   REQUIRE(cudaMemset2DAsync(reinterpret_cast<void*>(clean.data), clean.descriptor.pitch_bytes, value, clean.descriptor.row_bytes(), clean.descriptor.height, reinterpret_cast<cudaStream_t>(stream)) ==
           cudaSuccess);
  });
  static_cast<void>(runtime.CommitOutput(std::move(candidate)));
 }
};
void check_writer_pixels(const fs::path& path, unsigned char expected) {
 int width = 0, height = 0, channels = 0;
 std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(stbi_load(path.c_str(), &width, &height, &channels, 4), stbi_image_free);
 REQUIRE(pixels);
 REQUIRE(width == 7);
 REQUIRE(height == 5);
 for (int index = 0; index < width * height * 4; ++index) CHECK(pixels.get()[index] == expected);
}
}  // namespace
TEST_CASE("rendered image writer atomically publishes owned pixels and preserves completed files on failure", "[raster][cuda][rendered_image_writer]") {
 namespace gpu = mmltk::frameworks::gpu;
 RenderedImageFixture fixture;
 auto& context = fixture.device.context;
 auto& runtime = fixture.runtime;
 mmltk::backend::imaging::raster::RenderedImageWriter writer(context);
 const mmltk::testsupport::ScopedTempDir directory("rendered-image-writer");
 fixture.Fill(73);
 const auto destination = directory.path() / "samples" / "sample-7.png";
 CHECK(writer.Flush().empty());
 writer.Write(runtime.Borrow(), destination);
 CHECK_THROWS_AS(writer.Write(runtime.Borrow(), destination), std::logic_error);
 fixture.Fill(201);  // Source is reusable before the asynchronous file worker finishes.
 REQUIRE(writer.Flush() == destination);
 check_writer_pixels(destination, 73U);
 // Pre-existing staging entries belong to someone else, even if they point at
 // the incumbent. Failure may remove only this writer's exclusive attempt.
 const bool symlink = GENERATE(false, true);
 const auto occupied_partial = destination.string() + ".partial";
 if (symlink)
  fs::create_symlink(destination, occupied_partial);
 else {
  std::ofstream prior(occupied_partial);
  prior << "earlier staging";
 }
 mmltk::backend::imaging::raster::RenderedImageWriter failing_sink(context, [](const char* path, int w, int h, int channel_count, const void* data, int stride) {
  auto* stream = std::fopen("/dev/full", "wb");
  if (!stream) throw std::runtime_error("open PNG failure stream");
  return mmltk::backend::imaging::raster::detail::write_png_stream(stream, path, w, h, channel_count, data, stride);
 });
 failing_sink.Write(runtime.Borrow(), destination);
 CHECK_THROWS_AS(failing_sink.Flush(), std::system_error);
 CHECK(fs::exists(occupied_partial));
 if (symlink)
  CHECK(fs::read_symlink(occupied_partial) == destination);
 else {
  std::ifstream prior(occupied_partial);
  CHECK(std::string(std::istreambuf_iterator<char>(prior), {}) == "earlier staging");
 }
 CHECK(std::distance(fs::directory_iterator(destination.parent_path()), fs::directory_iterator{}) == 2);
 check_writer_pixels(destination, 73U);
 // Staging creation cannot traverse a regular file as its parent.
 const auto blocked_parent = directory.path() / "blocked";
 {
  std::ofstream blocked(blocked_parent);
  blocked << "existing";
 }
 const auto unopened = blocked_parent / "unopened.png";
 writer.Write(runtime.Borrow(), unopened);
 CHECK_THROWS(writer.Flush());
 CHECK(fs::is_regular_file(blocked_parent));
 // Actual exclusive-file opening fails after parent preparation succeeds.
 writer.Write(runtime.Borrow(), directory.path() / std::string(300U, 'p'));
 CHECK_THROWS_AS(writer.Flush(), std::system_error);
 const auto refused = directory.path() / "occupied.png";
 fs::create_directory(refused);
 writer.Write(runtime.Borrow(), refused);
 CHECK_THROWS(writer.Flush());
 CHECK(fs::is_regular_file(destination));
 CHECK(fs::is_directory(refused));
 CHECK_FALSE(fs::exists(refused.string() + ".partial"));
 CHECK(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator{}) == 3);
 mmltk::backend::imaging::raster::RenderedImageWriter refused_encoder(context, [](const char*, int, int, int, const void*, int) { return 0; });
 refused_encoder.Write(runtime.Borrow(), destination);
 CHECK_THROWS_AS(refused_encoder.Flush(), std::runtime_error);
 CHECK(fs::is_regular_file(destination));
 CHECK(fs::exists(occupied_partial));
 CHECK(std::distance(fs::directory_iterator(destination.parent_path()), fs::directory_iterator{}) == 2);
 writer.Write(runtime.Borrow(), destination);
 CHECK(writer.Flush() == destination);
 CHECK(fs::exists(occupied_partial));
 CHECK(std::distance(fs::directory_iterator(destination.parent_path()), fs::directory_iterator{}) == 2);
 writer.Write(runtime.Borrow(), directory.path() / "retry.png");
 CHECK(writer.Flush() == directory.path() / "retry.png");
}
TEST_CASE("rendered image writer shutdown retains engaged encoder pixels through atomic publication", "[raster][cuda][rendered_image_writer]") {
 namespace gpu = mmltk::frameworks::gpu;
 RenderedImageFixture fixture;
 auto& context = fixture.device.context;
 auto& runtime = fixture.runtime;
 mmltk::testsupport::TestGate encoding("rendered writer pending encode");
 auto writer =
  std::make_unique<mmltk::backend::imaging::raster::RenderedImageWriter>(context, [gate = encoding.receipt()](const char* path, int width, int height, int channels, const void* pixels, int stride) {
  gate.ArriveAndWait();
  return stbi_write_png(path, width, height, channels, pixels, stride);
 });
 mmltk::testsupport::ScopedTestCleanup release([&] { encoding.Release(); });
 const mmltk::testsupport::ScopedTempDir directory("writer-shutdown-output");
 const auto path = directory.path() / "sample.png";
 fixture.Fill(73);
 writer->Write(runtime.Borrow(), path);
 REQUIRE(encoding.WaitEntered(std::chrono::seconds(5)));
 std::promise<void> destroying;
 auto stopped = std::async(std::launch::async, [&] {
  destroying.set_value();
  writer.reset();
 });
 mmltk::testsupport::ScopedTestCleanup unblock([&] { encoding.Release(); });
 mmltk::testsupport::await_test_promise(destroying, "writer destruction entered");
 CHECK(stopped.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
 CHECK_FALSE(fs::exists(path));
 encoding.Release();
 mmltk::testsupport::await_test_future(stopped, "writer destruction drained", std::chrono::seconds(5));
 CHECK_FALSE(fs::exists(path.string() + ".partial"));
 CHECK(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator{}) == 1);
 check_writer_pixels(path, 73U);
}
namespace {
// Real glibc FILE operations reach these device-like streams. This exercises
// the default sink's fwrite/fflush/fclose results without replacing its encoder.
class FailingPngStream final {
public:
 enum class Failure { ShortWrite, Write, Flush, Close };
 explicit FailingPngStream(Failure failure) : failure_(failure) {}
 std::FILE* Open() {
  cookie_io_functions_t operations{};
  operations.write = &Write;
  operations.close = &Close;
  auto* stream = fopencookie(this, "w", operations);
  if (!stream) throw std::system_error(errno, std::generic_category(), "open PNG test stream");
  if (setvbuf(stream, buffer_.data(), failure_ == Failure::Flush ? _IOFBF : _IONBF, buffer_.size()) != 0) {
   std::fclose(stream);
   throw std::runtime_error("configure PNG test stream buffering");
  }
  return stream;
 }
 unsigned writes = 0;
 unsigned closes = 0;

private:
 static ssize_t Write(void* cookie, const char*, std::size_t count) noexcept {
  auto& self = *static_cast<FailingPngStream*>(cookie);
  ++self.writes;
  if (self.failure_ == Failure::Close) return static_cast<ssize_t>(count);
  errno = ENOSPC;
  return self.failure_ == Failure::ShortWrite ? static_cast<ssize_t>(count / 2U) : 0;
 }
 static int Close(void* cookie) noexcept {
  auto& self = *static_cast<FailingPngStream*>(cookie);
  ++self.closes;
  if (self.failure_ != Failure::Close) return 0;
  errno = EIO;
  return -1;
 }
 Failure failure_;
 std::array<char, 4096> buffer_{};
};
}  // namespace
TEST_CASE("default PNG sink checks byte writes buffered flush and close before success", "[raster][rendered_image_writer]") {
 using Failure = FailingPngStream::Failure;
 const auto failure = GENERATE(Failure::ShortWrite, Failure::Write, Failure::Flush, Failure::Close);
 FailingPngStream stream(failure);
 const std::array<unsigned char, 7U * 5U * 4U> pixels{};
 const std::string operation = failure == Failure::Flush ? "flush" : failure == Failure::Close ? "close" : "write";
 try {
  mmltk::backend::imaging::raster::detail::write_png_stream(stream.Open(), "sample.png.partial", 7, 5, 4, pixels.data(), 28);
  FAIL("failed PNG stream was accepted");
 } catch (const std::system_error& error) {
  CHECK(error.code().value() == (failure == Failure::Close ? EIO : ENOSPC));
  CHECK(std::string(error.what()).find(operation + " rendered PNG: sample.png.partial") != std::string::npos);
 }
 CHECK(stream.writes > 0U);
 CHECK(stream.closes == 1U);
}
TEST_CASE("checked PNG file encoder reports kernel write and flush failures", "[raster][rendered_image_writer]") {
 const int extent = GENERATE(1, 128);
 std::vector<std::uint8_t> pixels(static_cast<std::size_t>(extent * extent * 4));
 // Incompressible deterministic pixels exceed stdio buffering for the large
 // case; the one-pixel PNG instead reaches ENOSPC during explicit fflush.
 std::uint32_t state = 17U;
 for (auto& value : pixels) {
  state = state * 1664525U + 1013904223U;
  value = static_cast<std::uint8_t>(state >> 24U);
 }
 const std::string operation = extent == 1 ? "flush" : "write";
 try {
  mmltk::backend::imaging::raster::detail::write_png_file("/dev/full", extent, extent, 4, pixels.data(), extent * 4);
  FAIL("/dev/full accepted PNG bytes");
 } catch (const std::system_error& error) {
  CHECK(error.code().value() == ENOSPC);
  CHECK(std::string(error.what()).find(operation + " rendered PNG: /dev/full") != std::string::npos);
 }
}
TEST_CASE("PNG extent admission bounds stb arithmetic without allocating image storage", "[raster][rendered_image_writer]") {
 namespace detail = mmltk::backend::imaging::raster::detail;
 constexpr auto limit = static_cast<std::size_t>(std::numeric_limits<int>::max());
 CHECK_NOTHROW(detail::validate_png_extent(10000U, 10000U, 4U, 0U, "native.png"));
 CHECK_THROWS_WITH(detail::validate_png_extent(24000U, 24000U, 4U, 0U, "oversized.png"), "unsupported rendered PNG extent: oversized.png");
 CHECK_THROWS_AS(detail::validate_png_extent(0U, 1U, 4U, 0U, "empty.png"), std::invalid_argument);
 CHECK_THROWS_AS(detail::validate_png_extent(1U, 0U, 4U, 0U, "empty.png"), std::invalid_argument);
 CHECK_THROWS_AS(detail::validate_png_extent(1U, 1U, 5U, 0U, "channels.png"), std::invalid_argument);
 CHECK_THROWS_AS(detail::validate_png_extent(limit, 1U, 4U, 0U, "axis.png"), std::invalid_argument);
 CHECK_THROWS_AS(detail::validate_png_extent(std::numeric_limits<std::size_t>::max(), 1U, 4U, 0U, "axis.png"), std::invalid_argument);
 CHECK_THROWS_AS(detail::validate_png_extent(1U, 2U, 4U, limit, "stride.png"), std::invalid_argument);
 CHECK_THROWS_AS(detail::validate_png_extent(2U, 1U, 4U, 4U, "stride.png"), std::invalid_argument);
 const auto score_width = limit / 128U / 4U;
 CHECK_NOTHROW(detail::validate_png_extent(score_width, 1U, 4U, 0U, "score.png"));
 CHECK_THROWS_AS(detail::validate_png_extent(score_width + 1U, 1U, 4U, 0U, "score.png"), std::invalid_argument);
 const auto compressed_height = (((limit - 1U) / 2U - 121U) * 8U) / 45U;
 CHECK_NOTHROW(detail::validate_png_extent(1U, compressed_height, 4U, 0U, "compression.png"));
 CHECK_THROWS_AS(detail::validate_png_extent(1U, compressed_height + 1U, 4U, 0U, "compression.png"), std::invalid_argument);
 // The private stream boundary also rejects before stb can read even one
 // pixel, and closes its accepted stream on this exceptional path.
 FailingPngStream stream(FailingPngStream::Failure::Write);
 const std::array<unsigned char, 4> tiny{};
 CHECK_THROWS_AS(detail::write_png_stream(stream.Open(), "oversized.png", 24000, 24000, 4, tiny.data(), 96000), std::invalid_argument);
 CHECK(stream.writes == 0U);
 CHECK(stream.closes == 1U);
 const mmltk::testsupport::ScopedTempDir directory("invalid-png-extent");
 const auto path = directory.path() / "incumbent.png";
 {
  std::ofstream incumbent(path);
  incumbent << "incumbent bytes";
 }
 CHECK_THROWS_AS(detail::write_png_file(path.c_str(), 24000, 24000, 4, tiny.data(), 96000), std::invalid_argument);
 std::ifstream incumbent(path);
 CHECK(std::string(std::istreambuf_iterator<char>(incumbent), {}) == "incumbent bytes");
}
TEST_CASE("concurrent PNG writers retain independent staging and settle only their own attempt", "[raster][cuda][rendered_image_writer]") {
 namespace gpu = mmltk::frameworks::gpu;
 namespace raster = mmltk::backend::imaging::raster;
 const bool fail_first = GENERATE(false, true);
 RenderedImageFixture fixture;
 auto& context = fixture.device.context;
 auto& runtime = fixture.runtime;
 const mmltk::testsupport::ScopedTempDir directory("concurrent-png-output");
 const auto destination = directory.path() / "sample.png";
 mmltk::testsupport::TestGate first_encoding("first independent PNG encoder");
 std::promise<fs::path> pending_path;
 bool first_call = true;
 raster::RenderedImageWriter first(context, [&](const char* path, int w, int h, int channels, const void* data, int stride) {
  if (std::exchange(first_call, false)) {
   pending_path.set_value(path);
   first_encoding.receipt().ArriveAndWait();
   if (fail_first) return 0;
  }
  return raster::detail::write_png_file(path, w, h, channels, data, stride);
 });
 raster::RenderedImageWriter second(context);
 mmltk::testsupport::ScopedTestCleanup release([&] { first_encoding.Release(); });
 fixture.Fill(73);
 first.Write(runtime.Borrow(), destination);
 REQUIRE(first_encoding.WaitEntered(std::chrono::seconds(5)));
 const auto staging = mmltk::testsupport::await_test_promise(pending_path, "first PNG staging path");
 CHECK(staging != destination);
 CHECK(fs::is_regular_file(staging));
 fixture.Fill(201);
 second.Write(runtime.Borrow(), destination);
 REQUIRE(second.Flush() == destination);
 CHECK(fs::is_regular_file(staging));
 CHECK(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator{}) == 2);
 check_writer_pixels(destination, 201U);
 first_encoding.Release();
 if (fail_first)
  CHECK_THROWS_AS(first.Flush(), std::runtime_error);
 else
  CHECK(first.Flush() == destination);
 CHECK_FALSE(fs::exists(staging));
 CHECK(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator{}) == 1);
 check_writer_pixels(destination, fail_first ? 201U : 73U);
 first.Write(runtime.Borrow(), destination);
 CHECK(first.Flush() == destination);
 check_writer_pixels(destination, 201U);
 CHECK(std::distance(fs::directory_iterator(directory.path()), fs::directory_iterator{}) == 1);
}
