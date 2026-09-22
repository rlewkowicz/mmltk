#include "src/frameworks/transport/tests/support/browser_assets.h"
#include "src/frameworks/transport/browser_server.h"
#include "src/frameworks/transport/browser_server_lifecycle.h"
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <limits>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include "src/test_support/filesystem_test_utils.hpp"
namespace mmltk::frameworks::transport {
namespace {
struct CallbackFacts final {
 std::size_t opened = 0U;
 std::size_t records = 0U;
 std::size_t closed = 0U;
};
[[nodiscard]] BrowserServer::Callbacks callbacks(const std::shared_ptr<CallbackFacts>& facts) {
 return {
  .context = facts,
  .opened = [](void* context) noexcept { ++static_cast<CallbackFacts*>(context)->opened; },
  .record =
   [](void* context, std::span<const std::byte>) noexcept {
    ++static_cast<CallbackFacts*>(context)->records;
    return true;
   },
  .closed = [](void* context) noexcept { ++static_cast<CallbackFacts*>(context)->closed; },
 };
}
[[nodiscard]] BrowserOutputRecord output(const BrowserRecordPriority priority, const std::size_t bytes) {
 return {
  .bytes = mmltk::frameworks::serialization::wire::ByteBuffer(bytes, std::byte{0x2a}),
  .priority = priority,
 };
}
TEST_CASE("browser peer generations notify logical closure exactly once", "[frameworks][transport][browser][lifecycle]") {
 detail::BrowserPeerLifecycle peer;
 CHECK_FALSE(peer.begin_close());
 peer.opened(1U);
 CHECK(peer.generation == 1U);
 CHECK(peer.begin_close());
 CHECK_FALSE(peer.begin_close());
 peer.opened(2U);
 CHECK(peer.generation == 2U);
 CHECK(peer.begin_close());
 CHECK_FALSE(peer.begin_close());
}
TEST_CASE("browser output epochs keep Bootstrap first on attach and reconnect", "[frameworks][transport][browser][lifecycle]") {
 detail::BrowserOutputEpoch epoch;
 BrowserRecordRing ring;
 const auto publish = [&epoch, &ring](const bool owner_thread, const std::byte marker) {
  if (!epoch.admits(owner_thread)) return BrowserRecordPush::Dropped;
  return ring.push({
   .bytes = {marker},
   .priority = BrowserRecordPriority::Critical,
  });
 };
 const auto publish_diagnostic = [&publish](const std::byte marker) { return publish(true, marker); };
 const auto publish_worker = [&publish](const std::byte marker) { return publish(false, marker); };
 const auto require_markers = [&ring](const std::initializer_list<std::byte> markers) {
  for (const auto marker : markers) {
   const auto record = ring.pop();
   REQUIRE(record);
   CHECK(record->bytes.front() == marker);
  }
 };
 const auto finish_open = [&](const std::uint64_t generation, const std::byte accepted, const std::byte progress, const std::byte diagnostic, const std::byte worker) {
  epoch.finish_open(generation);
  require_markers({accepted});
  REQUIRE(ring.push({.bytes = {progress}, .priority = BrowserRecordPriority::Critical}) == BrowserRecordPush::Enqueued);
  REQUIRE(publish_diagnostic(diagnostic) == BrowserRecordPush::Enqueued);
  REQUIRE(publish_worker(worker) == BrowserRecordPush::Enqueued);
  require_markers({progress, diagnostic, worker});
 };
 CHECK(publish_worker(std::byte{0x10}) == BrowserRecordPush::Dropped);
 epoch.begin_open(1U);
 ring.clear();
 CHECK_FALSE(epoch.admits(false));
 REQUIRE(publish(true, std::byte{0x11}) == BrowserRecordPush::Enqueued);
 // Accepted socket copy precedes activation, including backpressure.
 finish_open(1U, std::byte{0x11}, std::byte{0x14}, std::byte{0x12}, std::byte{0x13});
 epoch.close();
 ring.clear();
 CHECK(publish_worker(std::byte{0x20}) == BrowserRecordPush::Dropped);
 epoch.begin_open(2U);
 ring.clear();
 REQUIRE(publish(true, std::byte{0x21}) == BrowserRecordPush::Enqueued);
 epoch.finish_open(1U);
 CHECK_FALSE(epoch.admits(false));
 finish_open(2U, std::byte{0x21}, std::byte{0x24}, std::byte{0x22}, std::byte{0x23});
}
TEST_CASE("browser server rejects output without an active peer epoch", "[frameworks][transport][browser][lifecycle]") {
 mmltk::testsupport::BrowserAssetDirectory assets{"mmltk-browser-server"};
 const auto facts = std::make_shared<CallbackFacts>();
 BrowserServer server;
 REQUIRE(server.start({.asset_root = assets.path(), .session_token = "test-capability"}, callbacks(facts)));
 REQUIRE(server.running());
 REQUIRE(server.page_url());
 REQUIRE(server.websocket_url());
 CHECK(server.publish(output(BrowserRecordPriority::Transient, 1U)) == BrowserRecordPush::Dropped);
 CHECK(server.queued_records() == 0U);
 CHECK(server.publish(output(BrowserRecordPriority::Transient, 0U)) == BrowserRecordPush::Dropped);
 CHECK(server.queued_records() == 0U);
 CHECK(server.publish(output(BrowserRecordPriority::Critical, 8U * 1024U * 1024U + 1U)) == BrowserRecordPush::ClosePeer);
 CHECK(server.queued_records() == 0U);
 server.stop();
 CHECK(server.publish(output(BrowserRecordPriority::Transient, 1U)) == BrowserRecordPush::Dropped);
 CHECK(server.publish(output(BrowserRecordPriority::Critical, 1U)) == BrowserRecordPush::ClosePeer);
 CHECK(server.queued_records() == 0U);
 server.run();
 CHECK_FALSE(server.running());
 CHECK_FALSE(server.connected());
 CHECK_FALSE(server.page_url());
 CHECK_FALSE(server.websocket_url());
 CHECK(server.queued_records() == 0U);
 CHECK(facts->closed == 0U);
 CHECK(server.publish(output(BrowserRecordPriority::Transient, 1U)) == BrowserRecordPush::Dropped);
 CHECK(server.publish(output(BrowserRecordPriority::Critical, 1U)) == BrowserRecordPush::ClosePeer);
}
TEST_CASE("browser output ceiling is configured independently of inbound admission", "[frameworks][transport][browser][limits]") {
 mmltk::testsupport::BrowserAssetDirectory assets{"mmltk-browser-output-ceiling"};
 const auto facts = std::make_shared<CallbackFacts>();
 BrowserServer server;
 CHECK_FALSE(server.start({.asset_root = assets.path(), .session_token = "test-capability", .maximum_output_bytes = 0U}, callbacks(facts)));
 CHECK_FALSE(server.start({.asset_root = assets.path(), .session_token = "test-capability", .maximum_output_bytes = std::numeric_limits<std::size_t>::max()}, callbacks(facts)));
 REQUIRE(server.start({.asset_root = assets.path(), .session_token = "test-capability", .maximum_output_bytes = 32U * 1024U * 1024U}, callbacks(facts)));
 CHECK(server.close());
}
TEST_CASE("off-owner close reports owner finalization obligation", "[frameworks][transport][browser][lifecycle]") {
 mmltk::testsupport::BrowserAssetDirectory assets{"mmltk-browser-server"};
 auto facts = std::make_shared<CallbackFacts>();
 const std::weak_ptr<CallbackFacts> released = facts;
 auto server = std::make_unique<BrowserServer>();
 REQUIRE(server->start({.asset_root = assets.path(), .session_token = "close-before-run"}, callbacks(facts)));
 server->close_peer();
 bool worker_finalized = true;
 std::thread worker([&server, &worker_finalized] { worker_finalized = server->close(); });
 worker.join();
 CHECK_FALSE(worker_finalized);
 facts.reset();
 CHECK_FALSE(released.expired());
 CHECK(server->close());
 CHECK(released.expired());
 CHECK_FALSE(server->running());
 CHECK_FALSE(server->connected());
 CHECK_FALSE(server->page_url());
 CHECK_FALSE(server->websocket_url());
 CHECK(server->queued_records() == 0U);
 CHECK(server->close());
 server->run();
 const auto second = std::make_shared<CallbackFacts>();
 CHECK_FALSE(server->start({.asset_root = assets.path(), .session_token = "one-shot-rejected"}, callbacks(second)));
 CHECK_FALSE(server->running());
 std::thread destroyer([server = std::move(server)]() mutable { server.reset(); });
 destroyer.join();
}
}  // namespace
}  // namespace mmltk::frameworks::transport
