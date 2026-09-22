#pragma once
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <catch2/catch_test_macros.hpp>
#include "src/test_support/async_test_utils.hpp"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/controller/presentation/visual_runtime_owner.h"
#include "src/frameworks/gpu/image_workspace.h"
namespace mmltk::controller::visual_test_support {
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
[[nodiscard]] inline auto settle_visual_on_exit(detail::VisualRuntimeOwner& owner, std::promise<void>& release) {
 return mmltk::testsupport::ScopedTestCleanup{[&owner, &release] {
  mmltk::testsupport::release_test_promise(release);
  owner.RequestStop();
  owner.StopAndWait();
 }};
}
struct ProducerWorkspaceRequest final {
 std::shared_ptr<mmltk::frameworks::gpu::ImageWorkspace> workspace{};
 mmltk::frameworks::gpu::ImageWorkspaceContent content{};
 std::future<void> ready{};
 std::uintptr_t stream = 0U;
 template <class Producer>
 void Request(Producer& producer) {
  const auto observed = producer.ObserveWorkspace();
  content = {observed.product_owner, observed.product_revision};
  REQUIRE(content.valid());
  auto completed = std::make_shared<std::promise<void>>();
  ready = completed->get_future();
  producer.RequestWorkspace({.product_owner = content.owner, .product_revision = content.revision, .destination = workspace, .ready = [completed] { completed->set_value(); }});
 }
 template <class Producer>
 void CheckCompleted(Producer& producer) {
  mmltk::testsupport::await_test_future(ready, "producer workspace completion");
  REQUIRE(workspace->Contains(content));
  const auto observed = producer.ObserveWorkspace();
  CHECK(observed.product_owner == content.owner);
  CHECK(observed.product_revision == content.revision);
  auto borrowed = producer.BorrowWorkspace();
  REQUIRE(borrowed.valid());
  CHECK(borrowed.identity() == workspace->identity());
  CHECK(borrowed.revision() == content.revision);
  CHECK(borrowed.plane().data == workspace->plane(borrowed.plane().descriptor.width, borrowed.plane().descriptor.height).data);
 }
};
}  // namespace mmltk::controller::visual_test_support
