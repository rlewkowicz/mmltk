#include "src/frameworks/gpu/cuda_context_scope.h"
#include "src/frameworks/gpu/tests/vulkan_workspace_fixture.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/system_image_runtime.h"
#include "src/frameworks/gpu/image_failure.h"
#include "src/frameworks/gpu/imported_image_buffer.h"
#include "src/frameworks/gpu/external_graphics_timeline.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <algorithm>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <future>
#include <memory>
#include <optional>
#include <limits>
#include <cstring>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <sys/mman.h>
#include <vector>
#include "src/frameworks/gpu/tests/fake_image_backend.h"
namespace mmltk::frameworks::gpu {
namespace {
using test_support::FakeImageBackend;
using test_support::make_clean_semantic_runtime;
using test_support::RuntimeFactory;
class CleanSemanticRuntime final {
public:
 explicit CleanSemanticRuntime(const std::size_t output_buffer_count = 2U)
     : backend(std::make_shared<FakeImageBackend>()),
       runtime{{
        .device = 0,
        .backend = backend,
        .output_layout = ImageProductLayout::CleanAndSemantic,
        .output_buffer_count = output_buffer_count,
       }} {}
 std::shared_ptr<FakeImageBackend> backend;
 SystemImageRuntime runtime;
};
TEST_CASE("product candidates preserve exact committed planes until readers release them") {
 using namespace std::chrono_literals;
 CleanSemanticRuntime scenario;
 auto& runtime = scenario.runtime;
 runtime.Publish(8U, 8U, [](const auto clean, const auto semantic, auto) {
  std::memset(reinterpret_cast<void*>(clean.data), 0x21, clean.descriptor.pitch_bytes * clean.descriptor.height);
  std::memset(reinterpret_cast<void*>(semantic.data), 0x43, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
 });
 mmltk::testsupport::TestGate incumbent_gate{"incumbent product reader"};
 std::promise<std::uint64_t> incumbent_ready;
 auto incumbent = std::async(std::launch::async, [&] {
  auto read = runtime.Borrow();
  incumbent_ready.set_value(read.valid() ? read.plane(0U).revision() : 0U);
  incumbent_gate.receipt().ArriveAndWait();
  return *reinterpret_cast<const std::uint8_t*>(read.plane(1U).plane().data);
 });
 mmltk::testsupport::ScopedTestCleanup release_incumbent{[&] { incumbent_gate.Release(); }};
 const auto incumbent_revision = mmltk::testsupport::await_test_promise(incumbent_ready, "incumbent revision");
 REQUIRE(incumbent_revision != 0U);
 REQUIRE(incumbent_gate.WaitEntered(1s));
 auto candidate = runtime.AcquireOutput({}, runtime.Completed());
 REQUIRE(candidate.valid());
 runtime.Publish(
  candidate, 8U, 8U, [](const auto, const auto semantic, auto) { std::memset(reinterpret_cast<void*>(semantic.data), 0x65, semantic.descriptor.pitch_bytes * semantic.descriptor.height); });
 const auto candidate_revision = candidate.revision();
 CHECK(candidate_revision > incumbent_revision);
 CHECK(runtime.Borrow().plane(0U).revision() == incumbent_revision);
 runtime.CommitOutput(std::move(candidate));
 auto committed = runtime.Borrow();
 REQUIRE(committed.valid());
 CHECK(committed.plane(0U).revision() == candidate_revision);
 CHECK(*reinterpret_cast<const std::uint8_t*>(committed.plane(0U).plane().data) == 0x21U);
 CHECK(*reinterpret_cast<const std::uint8_t*>(committed.plane(1U).plane().data) == 0x65U);
 SystemImageRuntime::CompletedOutput unavailable_baseline;
 auto reservation = std::async(std::launch::async, [&] { return runtime.TryAcquireOutput(unavailable_baseline).valid(); });
 REQUIRE_FALSE(mmltk::testsupport::await_test_future(reservation, "two-product reader reservation"));
 std::stop_source stop;
 auto admission = std::async(std::launch::async, [&runtime, token = stop.get_token()] { return runtime.AcquireOutput(token); });
 mmltk::testsupport::ScopedTestCleanup stop_admission{[&] { stop.request_stop(); }};
 incumbent_gate.Release();
 CHECK(mmltk::testsupport::await_test_future(incumbent, "incumbent reader release") == 0x43U);
 REQUIRE(admission.wait_for(1s) == std::future_status::ready);
 CHECK(admission.get().valid());
}
TEST_CASE("completed products retain exact pixels and can be selected again") {
 auto backend = std::make_shared<FakeImageBackend>();
 SystemImageRuntime runtime{{
  .device = 0,
  .backend = backend,
  .output_buffer_count = 3U,
 }};
 auto first_candidate = runtime.AcquireOutput();
 REQUIRE(first_candidate.valid());
 runtime.Publish(first_candidate, 8U, 8U, [](const auto clean, const auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 0x31, clean.descriptor.pitch_bytes * clean.descriptor.height); });
 auto first = runtime.CommitOutput(std::move(first_candidate));
 REQUIRE(first.valid());
 auto second_candidate = runtime.AcquireOutput();
 REQUIRE(second_candidate.valid());
 runtime.Publish(second_candidate, 8U, 8U, [](const auto clean, const auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 0x52, clean.descriptor.pitch_bytes * clean.descriptor.height); });
 auto second = runtime.CommitOutput(std::move(second_candidate));
 REQUIRE(second.valid());
 REQUIRE(runtime.Borrow().valid());
 CHECK(*reinterpret_cast<const std::uint8_t*>(runtime.Borrow().plane(0U).plane().data) == 0x52U);
 runtime.SelectOutput(first);
 auto selected = runtime.Borrow();
 REQUIRE(selected.valid());
 CHECK(selected.plane(0U).revision() == first.revision());
 CHECK(*reinterpret_cast<const std::uint8_t*>(selected.plane(0U).plane().data) == 0x31U);
 auto later = second.Borrow();
 REQUIRE(later.valid());
 CHECK(*reinterpret_cast<const std::uint8_t*>(later.plane(0U).plane().data) == 0x52U);
 auto remaining = std::async(std::launch::async, [&] { return runtime.AcquireOutput(); });
 CHECK(mmltk::testsupport::await_test_future(remaining, "remaining product slot").valid());
}
TEST_CASE("failed candidate growth retains the committed product and later initializes every plane") {
 CleanSemanticRuntime scenario;
 auto& backend = scenario.backend;
 auto& runtime = scenario.runtime;
 runtime.Publish(8U, 8U, [](const auto clean, const auto semantic, auto) {
  std::memset(reinterpret_cast<void*>(clean.data), 0x17, clean.descriptor.pitch_bytes * clean.descriptor.height);
  std::memset(reinterpret_cast<void*>(semantic.data), 0x29, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
 });
 const auto committed_revision = runtime.OutputFacts().revision;
 {
  auto candidate = runtime.AcquireOutput({}, runtime.Completed());
  REQUIRE(candidate.valid());
  backend->FailAfter(FakeImageBackend::FailurePoint::AllocatePlane, 1U);
  CHECK_THROWS(runtime.Publish(candidate, 16U, 16U, [](auto, auto, auto) {}));
 }
 auto retained = runtime.Borrow();
 REQUIRE(retained.valid());
 CHECK(retained.plane(0U).revision() == committed_revision);
 CHECK(*reinterpret_cast<const std::uint8_t*>(retained.plane(0U).plane().data) == 0x17U);
 retained = {};
 auto replacement = runtime.AcquireOutput({}, runtime.Completed());
 REQUIRE(replacement.valid());
 runtime.Publish(replacement, 16U, 16U, [](const auto clean, const auto, auto) { *reinterpret_cast<std::uint8_t*>(clean.data) = 0x7BU; });
 runtime.CommitOutput(std::move(replacement));
 auto completed = runtime.Borrow();
 REQUIRE(completed.valid());
 CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(0U).plane().data) == 0x7BU);
 CHECK(*(reinterpret_cast<const std::uint8_t*>(completed.plane(0U).plane().data) + 1U) == 0U);
 CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(1U).plane().data) == 0U);
}
TEST_CASE("candidate baselines remain exact across cached selection and handle moves") {
 auto backend = std::make_shared<FakeImageBackend>();
 SystemImageRuntime runtime{{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 3U}};
 const auto publish = [&](std::uint8_t value) {
  auto candidate = runtime.AcquireOutput();
  runtime.Publish(candidate, 8U, 8U, [value](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), value, clean.descriptor.pitch_bytes * clean.descriptor.height); });
  return runtime.CommitOutput(std::move(candidate));
 };
 auto first = publish(0x19U);
 auto second = publish(0x37U);
 auto copied = second;
 auto moved = std::move(copied);
 CHECK_FALSE(copied.valid());
 CHECK(moved.revision() == second.revision());
 auto candidate = runtime.AcquireOutput({}, std::move(moved));
 auto working = std::move(candidate);
 CHECK_FALSE(candidate.valid());
 runtime.SelectOutput(first);
 runtime.Publish(working, 8U, 8U, [](auto, auto semantic, auto) { std::memset(reinterpret_cast<void*>(semantic.data), 0x55, semantic.descriptor.pitch_bytes * semantic.descriptor.height); });
 CHECK(runtime.Completed().revision() == first.revision());
 auto completed = runtime.CommitOutput(std::move(working));
 CHECK_FALSE(working.valid());
 auto read = completed.Borrow();
 REQUIRE(read.valid());
 CHECK(*reinterpret_cast<const std::uint8_t*>(read.plane(0U).plane().data) == 0x37U);
 CHECK(*reinterpret_cast<const std::uint8_t*>(read.plane(1U).plane().data) == 0x55U);
 CHECK(read.plane(0U).revision() == completed.revision());
 CHECK(read.plane(1U).revision() == completed.revision());
 CHECK_THROWS_AS(runtime.SelectOutput({}), std::invalid_argument);
 CHECK_THROWS_AS(runtime.CommitOutput({}), std::invalid_argument);
 SystemImageRuntime foreign{{.device = 0, .backend = backend}};
 CHECK_THROWS_AS(foreign.SelectOutput(first), std::invalid_argument);
 CHECK_THROWS_AS(foreign.AcquireOutput({}, first), std::invalid_argument);
 auto foreign_candidate = foreign.AcquireOutput();
 CHECK_THROWS_AS(runtime.Publish(foreign_candidate, 8U, 8U, [](auto, auto, auto) {}), std::invalid_argument);
 CHECK_THROWS_AS(runtime.CommitOutput(std::move(foreign_candidate)), std::invalid_argument);
 CHECK(foreign_candidate.valid());
}
TEST_CASE("clean-only candidate preservation excludes invalid semantics and rolls back without disturbing readers") {
 auto backend = std::make_shared<FakeImageBackend>();
 SystemImageRuntime runtime{{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 4U}};
 // CLEANUP-IGNORE: This publication establishes rollback/source-watch evidence, unlike the fixture's failed-growth baseline.
 runtime.Publish(8U, 8U, [](auto clean, auto semantic, auto) {
  std::memset(reinterpret_cast<void*>(clean.data), 0x17, clean.descriptor.pitch_bytes * clean.descriptor.height);
  std::memset(reinterpret_cast<void*>(semantic.data), 0x29, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
 });
 auto baseline = runtime.Completed();
 mmltk::testsupport::TestGate reader_gate{"clean-only rollback reader"};
 std::promise<void> reader_ready;
 auto reader = std::async(std::launch::async, [&] {
  auto read = baseline.Borrow();
  backend->watched_copy_source.store(read.plane(1U).plane().data);
  reader_ready.set_value();
  reader_gate.receipt().ArriveAndWait();
  return *reinterpret_cast<const std::uint8_t*>(read.plane(1U).plane().data);
 });
 mmltk::testsupport::ScopedTestCleanup release_reader{[&] { reader_gate.Release(); }};
 mmltk::testsupport::await_test_promise(reader_ready, "clean-only source custody");
 const auto copied = backend->same_copies.load();
 {
  auto candidate = runtime.AcquireOutput({}, baseline, ImagePlanePreservation::Clean);
  CHECK_THROWS_WITH(runtime.Publish(candidate, 8U, 8U,
                     [](auto, auto semantic, auto) {
                      std::memset(reinterpret_cast<void*>(semantic.data), 0x58, semantic.descriptor.pitch_bytes * semantic.descriptor.height);
                      throw std::runtime_error("semantic preparation failed");
                     }),
   "semantic preparation failed");
 }
 CHECK(runtime.Completed().revision() == baseline.revision());
 CHECK(backend->same_copies.load() == copied + 1U);
 CHECK(backend->watched_source_copies.load() == 0U);
 auto candidate = runtime.AcquireOutput({}, baseline, ImagePlanePreservation::Clean);
 runtime.Publish(candidate, 8U, 8U, [](auto, auto semantic, auto) { std::memset(reinterpret_cast<void*>(semantic.data), 0x68, semantic.descriptor.pitch_bytes * semantic.descriptor.height); });
 auto completed = runtime.CommitOutput(std::move(candidate)).Borrow();
 CHECK(backend->same_copies.load() == copied + 2U);
 CHECK(backend->watched_source_copies.load() == 0U);
 reader_gate.Release();
 CHECK(mmltk::testsupport::await_test_future(reader, "clean-only reader release") == 0x29U);
 CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(0U).plane().data) == 0x17U);
 CHECK(*reinterpret_cast<const std::uint8_t*>(completed.plane(1U).plane().data) == 0x68U);
}
TEST_CASE("single-slot candidates expose only committed selection") {
 auto backend = std::make_shared<FakeImageBackend>();
 SystemImageRuntime runtime{{.device = 0, .backend = backend}};
 const auto absent = [&] {
  CHECK_FALSE(runtime.Completed().valid());
  CHECK(runtime.OutputFacts().revision == 0U);
  CHECK_FALSE(runtime.Borrow().valid());
 };
 absent();
 runtime.Publish(8U, 8U, [](auto, auto, auto) {});
 const auto original = runtime.OutputFacts().revision;
 {
  auto candidate = runtime.AcquireOutput({}, runtime.Completed());
  absent();
 }
 CHECK(runtime.Completed().revision() == original);
 CHECK(runtime.OutputFacts().revision == original);
 CHECK(runtime.Borrow().valid());
 {
  auto candidate = runtime.AcquireOutput({}, runtime.Completed());
  absent();
  runtime.Publish(candidate, 8U, 8U, [](auto, auto, auto) {});
  absent();
 }
 absent();
 auto candidate = runtime.AcquireOutput();
 absent();
 runtime.Publish(candidate, 8U, 8U, [](auto, auto, auto) {});
 absent();
 auto completed = runtime.CommitOutput(std::move(candidate));
 CHECK(completed.revision() > original);
 CHECK(runtime.Completed().revision() == completed.revision());
 CHECK(runtime.OutputFacts().revision == completed.revision());
 CHECK(runtime.Borrow().plane(0U).revision() == completed.revision());
 CHECK(backend->planes_allocated == 1U);
}
TEST_CASE("quarantined cached products reject reads and selection while retaining physical custody") {
 auto backend = std::make_shared<FakeImageBackend>();
 auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 2U});
 runtime->Publish(8U, 8U, [](auto, auto, auto) {});
 auto healthy = runtime->Completed();
 runtime->Publish(8U, 8U, [](auto, auto, auto) {});
 auto cached = runtime->Completed();
 auto copy = cached;
 auto borrowed = cached.Borrow();
 auto plane = std::move(borrowed).TakePlane(1U);
 borrowed = {};
 plane.Quarantine();
 CHECK_FALSE(cached.valid());
 CHECK_FALSE(copy.valid());
 CHECK(cached.revision() == 0U);
 CHECK_FALSE(cached.Borrow().valid());
 CHECK_FALSE(runtime->Completed().valid());
 CHECK(runtime->OutputFacts().revision == 0U);
 CHECK_FALSE(runtime->Borrow().valid());
 CHECK_THROWS_AS(runtime->SelectOutput(cached), std::invalid_argument);
 CHECK_THROWS_WITH(runtime->AcquireOutput(), "image product storage is quarantined");
 runtime->SelectOutput(healthy);
 CHECK(runtime->Completed().revision() == healthy.revision());
 CHECK(runtime->Borrow().valid());
 runtime.reset();
 CHECK(backend->planes_freed == 0U);
 healthy = {};
 CHECK(backend->planes_freed == 2U);
 cached = {};
 copy = {};
 CHECK(backend->planes_freed == 2U);
 CHECK(backend->contexts_destroyed == 0U);
 plane = {};
 CHECK(backend->planes_freed == 4U);
 CHECK(backend->contexts_destroyed == 1U);
}
TEST_CASE("single-slot semantic replacement reuses clean pixels after its detached final reader") {
 using namespace std::chrono_literals;
 auto backend = std::make_shared<FakeImageBackend>();
 SystemImageRuntime runtime{{.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic}};
 runtime.Publish(8U, 8U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 0x26, clean.descriptor.pitch_bytes * clean.descriptor.height); });
 auto baseline = runtime.Completed();
 auto borrowed = baseline.Borrow();
 auto plane = std::move(borrowed).TakePlane(0U);
 borrowed = {};
 auto reservation = std::async(std::launch::async, [&] { return runtime.TryAcquireOutput(baseline).valid(); });
 REQUIRE_FALSE(mmltk::testsupport::await_test_future(reservation, "held-reader reservation"));
 std::stop_source stop;
 auto admission = std::async(std::launch::async, [&runtime, baseline = std::move(baseline), token = stop.get_token()]() mutable { return runtime.AcquireOutput(token, std::move(baseline)); });
 mmltk::testsupport::ScopedTestCleanup stop_admission{[&] { stop.request_stop(); }};
 plane = {};
 const auto status = admission.wait_for(1s);
 if (status != std::future_status::ready) stop.request_stop();
 REQUIRE(status == std::future_status::ready);
 auto candidate = admission.get();
 REQUIRE(candidate.valid());
 runtime.Publish(candidate, 8U, 8U, [](auto, auto semantic, auto) { std::memset(reinterpret_cast<void*>(semantic.data), 0x48, semantic.descriptor.pitch_bytes * semantic.descriptor.height); });
 auto completed = runtime.CommitOutput(std::move(candidate));
 auto read = completed.Borrow();
 REQUIRE(read.valid());
 CHECK(*reinterpret_cast<const std::uint8_t*>(read.plane(0U).plane().data) == 0x26U);
 CHECK(*reinterpret_cast<const std::uint8_t*>(read.plane(1U).plane().data) == 0x48U);
 CHECK(backend->planes_allocated == 2U);
 CHECK(backend->same_copies == 0U);
}
TEST_CASE("retained completed handles bound admission and stopping releases its wait") {
 using namespace std::chrono_literals;
 auto backend = std::make_shared<FakeImageBackend>();
 SystemImageRuntime runtime{{.device = 0, .backend = backend}};
 runtime.Publish(8U, 8U, [](auto, auto, auto) {});
 auto retained = runtime.Completed();
 SystemImageRuntime::CompletedOutput unavailable_baseline;
 REQUIRE_FALSE(runtime.TryAcquireOutput(unavailable_baseline).valid());
 // The public reservation attempt proves capacity pressure. Cancellation
 // must settle with custody still held whether it wins before or after the
 // private condition-variable wait registers; no scheduling delay proves
 // which interleaving occurred.
 std::stop_source stop;
 auto waiting = std::async(std::launch::async, [&] { return runtime.AcquireOutput(stop.get_token()); });
 mmltk::testsupport::ScopedTestCleanup stop_admission{[&] { stop.request_stop(); }};
 stop.request_stop();
 REQUIRE(waiting.wait_for(1s) == std::future_status::ready);
 CHECK_FALSE(waiting.get().valid());
 CHECK(retained.valid());
 CHECK(runtime.OutputFacts().revision == retained.revision());
 CHECK(backend->planes_allocated == 1U);
}
TEST_CASE("completed custody retains only its slot and context after pool retirement") {
 auto backend = std::make_shared<FakeImageBackend>();
 auto runtime = std::make_unique<SystemImageRuntime>(SystemImageRuntimeConfig{.device = 0, .backend = backend, .output_buffer_count = 2U});
 runtime->Publish(8U, 8U, [](auto, auto, auto) {});
 auto retained = runtime->Completed();
 runtime->Publish(8U, 8U, [](auto, auto, auto) {});
 runtime->SetOutputAvailableSink([] { throw std::runtime_error("notification sink failure"); });
 auto copy = retained;
 copy = {};
 runtime.reset();
 CHECK(backend->planes_freed == 1U);
 CHECK(backend->contexts_destroyed == 0U);
 REQUIRE(retained.Borrow().valid());
 retained = {};
 CHECK(backend->planes_freed == 2U);
 CHECK(backend->contexts_destroyed == 1U);
}
TEST_CASE("producer sequences reject zero and exhaustion without overwriting completed pixels") {
 CHECK_THROWS_AS(ImageProductRevisionSequence(0U), std::invalid_argument);
 auto backend = std::make_shared<FakeImageBackend>();
 auto revisions = std::make_shared<ImageProductRevisionSequence>(std::numeric_limits<std::uint64_t>::max() - 1U);
 SystemImageRuntime runtime{{.device = 0, .backend = backend, .product_revisions = revisions}};
 runtime.Publish(8U, 8U, [](auto, auto, auto) {});
 const auto revision = runtime.OutputFacts().revision;
 CHECK_THROWS_AS(runtime.Publish(8U, 8U, [](auto, auto, auto) {}), std::overflow_error);
 CHECK(runtime.Completed().revision() == revision);
 SystemImageRuntime replacement{{.device = 0, .backend = backend, .product_revisions = revisions}};
 CHECK_THROWS_AS(replacement.Publish(8U, 8U, [](auto, auto, auto) {}), std::overflow_error);
}
TEST_CASE("failed product completion invalidates the whole transaction") {
 auto backend = std::make_shared<FakeImageBackend>();
 auto source = make_clean_semantic_runtime(backend);
 source.Publish(8U, 8U, [](auto, auto, auto) {});
 auto receiver = make_clean_semantic_runtime(backend);
 backend->FailAfter(FakeImageBackend::FailurePoint::RecordEvent);
 CHECK_THROWS(receiver.CopyFrom(source.Borrow()));
 CHECK_FALSE(receiver.Borrow().valid());
 receiver.Publish(8U, 8U, [](auto, auto, auto) {});
 backend->FailAfter(FakeImageBackend::FailurePoint::SynchronizeStream);
 CHECK_THROWS(receiver.CopyFrom(source.Borrow()));
 CHECK_FALSE(receiver.Borrow().valid());
}
TEST_CASE("retained candidates expose allocation-local storage without clear or baseline copy", "[gpu][product][retained]") {
 auto backend = std::make_shared<FakeImageBackend>();
 SystemImageRuntime runtime({.device = 0, .backend = backend, .output_layout = ImageProductLayout::CleanAndSemantic, .output_buffer_count = 3U});
 const auto fill = [](const std::uint8_t value) {
  return [value](auto clean, auto semantic, auto) {
   for (auto plane : {clean, semantic}) std::memset(reinterpret_cast<void*>(plane.data), value, plane.descriptor.pitch_bytes * plane.descriptor.height);
  };
 };
 SystemImageRuntime::CompletedOutput baseline;
 auto first = runtime.TryAcquireOutput(baseline);
 CHECK(first.allocations()[0].identity == 0U);
 runtime.PublishRetained(first, 8U, 8U, fill(17U));
 const auto first_allocation = first.allocations();
 auto gallery = runtime.CommitOutput(std::move(first));
 auto second = runtime.TryAcquireOutput(baseline);
 runtime.PublishRetained(second, 8U, 8U, fill(33U));
 auto detail = runtime.CommitOutput(std::move(second));
 auto third = runtime.TryAcquireOutput(baseline);
 runtime.PublishRetained(third, 8U, 8U, fill(49U));
 auto selected = runtime.CommitOutput(std::move(third));
 auto held = gallery.Borrow();
 gallery = {};
 CHECK_FALSE(runtime.TryAcquireOutput(baseline).valid());
 held = {};
 auto reused = runtime.TryAcquireOutput(baseline);
 REQUIRE(reused.valid());
 CHECK(reused.allocations() == first_allocation);
 runtime.PublishRetained(reused, 8U, 8U, [](auto clean, auto semantic, auto) {
  CHECK(*reinterpret_cast<const std::uint8_t*>(clean.data) == 17U);
  CHECK(*reinterpret_cast<const std::uint8_t*>(semantic.data) == 17U);
  *reinterpret_cast<std::uint8_t*>(clean.data) = 71U;
 });
 CHECK(reused.allocations() == first_allocation);
 gallery = runtime.CommitOutput(std::move(reused));
 CHECK(backend->plane_clears == 0U);
 CHECK(backend->same_copies == 0U);
 runtime.SelectOutput(detail);
 CHECK(runtime.Completed().revision() == detail.revision());
 gallery = {};
 auto grown = runtime.TryAcquireOutput(baseline);
 REQUIRE(grown.allocations() == first_allocation);
 runtime.PublishRetained(grown, 16U, 9U, fill(85U));
 CHECK(grown.allocations()[0].owner == first_allocation[0].owner);
 CHECK(grown.allocations()[0].identity != first_allocation[0].identity);
 CHECK(grown.allocations()[0].width == 16U);
 CHECK(grown.allocations()[0].height == 9U);
 CHECK(runtime.Completed().revision() == detail.revision());
}
TEST_CASE("failed retained publication preserves the selected product and exposes touched candidate storage", "[gpu][product][retained]") {
 auto backend = std::make_shared<FakeImageBackend>();
 SystemImageRuntime runtime({.device = 0, .backend = backend, .output_buffer_count = 2U});
 runtime.Publish(4U, 4U, [](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), 7, clean.descriptor.pitch_bytes * clean.descriptor.height); });
 const auto selected = runtime.Completed();
 SystemImageRuntime::CompletedOutput baseline;
 auto candidate = runtime.TryAcquireOutput(baseline);
 CHECK_THROWS(runtime.PublishRetained(candidate, 4U, 4U, [](auto clean, auto, auto) {
  *reinterpret_cast<std::uint8_t*>(clean.data) = 123U;
  throw std::runtime_error("partial atlas write");
 }));
 const auto allocation = candidate.allocations();
 CHECK(candidate.revision() == 0U);
 CHECK(runtime.Completed().revision() == selected.revision());
 candidate = {};
 candidate = runtime.TryAcquireOutput(baseline);
 CHECK(candidate.allocations() == allocation);
 runtime.PublishRetained(candidate, 4U, 4U, [](auto clean, auto, auto) {
  CHECK(*reinterpret_cast<const std::uint8_t*>(clean.data) == 123U);
  std::memset(reinterpret_cast<void*>(clean.data), 31, clean.descriptor.pitch_bytes * clean.descriptor.height);
 });
 CHECK(runtime.CommitOutput(std::move(candidate)).valid());
}
TEST_CASE("held current display leaves replaceable overflow and exact readers close both slots", "[gpu][product][retained]") {
 auto backend = std::make_shared<FakeImageBackend>();
 SystemImageRuntime runtime({.device = 0, .backend = backend, .output_buffer_count = 2U});
 const auto fill = [](std::uint8_t value) {
  return [value](auto clean, auto, auto) { std::memset(reinterpret_cast<void*>(clean.data), value, clean.descriptor.pitch_bytes * clean.descriptor.height); };
 };
 runtime.Publish(4U, 4U, fill(11U));
 auto current = runtime.Borrow();
 const auto current_pointer = current.plane(0U).plane().data;
 auto baseline = runtime.Completed();
 auto overflow = runtime.TryAcquireOutput(baseline);
 REQUIRE(overflow.valid());
 runtime.Publish(overflow, 4U, 4U, fill(22U));
 baseline = runtime.CommitOutput(std::move(overflow));
 const auto overflow_allocation = baseline.ObserveWorkspace().product_owner;
 const auto copies = backend->same_copies.load();
 auto replacement = runtime.TryAcquireOutput(baseline);
 REQUIRE(replacement.valid());
 runtime.Publish(replacement, 4U, 4U, fill(33U));
 baseline = runtime.CommitOutput(std::move(replacement));
 CHECK(baseline.ObserveWorkspace().product_owner == overflow_allocation);
 CHECK(backend->same_copies == copies);
 CHECK(*reinterpret_cast<const std::uint8_t*>(current_pointer) == 11U);
 auto encoded = baseline.Borrow();
 REQUIRE(encoded.valid());
 CHECK_FALSE(runtime.TryAcquireOutput(baseline).valid());
 REQUIRE(baseline.valid());
 encoded = {};
 replacement = runtime.TryAcquireOutput(baseline);
 REQUIRE(replacement.valid());
 replacement = {};
 current = {};
 CHECK(runtime.TryAcquireOutput(baseline).valid());
}
}  // namespace
}  // namespace mmltk::frameworks::gpu
