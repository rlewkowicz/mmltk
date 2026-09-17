#include <catch2/catch_test_macros.hpp>
#include "src/frameworks/gpu/system_image_runtime.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>
#include "src/controller/browser/application_materializer.h"
#include "src/controller/shell/application_system_storage.h"
#include "src/frameworks/gpu/tests/fake_image_backend.h"
#include "src/common/system/cpu_affinity.h"
namespace mmltk::controller::shell {
namespace {
using mmltk::frameworks::gpu::test_support::FakeImageBackend;
using mmltk::frameworks::gpu::test_support::RuntimeFactory;
using namespace std::chrono_literals;
struct ShellWarmProbe final {
    std::atomic<std::size_t> calls{0U};
    bool fail = false;
    std::promise<void> completed;
};
class ShellWarmAlgorithm final : public UpscaleAlgorithm {
   public:
    void Semantics(mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t) override {}
    explicit ShellWarmAlgorithm(std::shared_ptr<ShellWarmProbe> probe) : probe_(std::move(probe)) {}
    void Warm() override {
        probe_->calls.fetch_add(1U, std::memory_order_acq_rel);
        if (probe_->fail) throw std::runtime_error("shell warm failure");
        probe_->completed.set_value();
    }
    void Run(UpscaleKernel, mmltk::frameworks::gpu::ImagePlaneView, mmltk::frameworks::gpu::ImagePlaneView, std::uintptr_t,
             const std::function<bool()>&) override {}

   private:
    std::shared_ptr<ShellWarmProbe> probe_;
};
[[nodiscard]] UpscaleSystem shell_upscale(const std::shared_ptr<FakeImageBackend>& backend, const std::shared_ptr<ShellWarmProbe>& probe,
                                          SystemEventSink<UpscaleSystem::event_type> events = {}) {
    return UpscaleSystem{
        {.device = 0, .maximum_width = 1024U, .maximum_height = 1024U},
        RuntimeFactory(
            0, backend, mmltk::frameworks::gpu::ImageProductLayout::Clean, [probe] { return std::make_unique<ShellWarmAlgorithm>(probe); }, 4U),
        [](const VisualFrame&) { return VisualDocumentRead{}; },
        std::move(events)};
}
[[nodiscard]] ExploreSystem::event_type ready_explore_event() {
    return ExploreChanged{ExploreSnapshot{
        .revision = 7U,
        .ready = true,
        .dataset = {.image_count = 3U, .image_width = 80U, .image_height = 45U},
        .viewport = {.extent = {80U, 40U}, .columns = 2U},
        .frame = visual_frame({PresentationSourceKind::Explore, 1U}, {80U, 40U}, 4U),
    }};
}
class ExploreRouteHarness final {
   public:
    explicit ExploreRouteHarness(UpscaleSystem& upscale)
        : application_events_([this](browser::SystemEvent event) { forwarded_.push_back(std::move(event)); }),
          route_(make_explore_upscale_event_sink(application_events_, upscale)),
          ready_(ready_explore_event()),
          expected_(std::visit([](const auto& event) { return browser::encode_system_event<&ApplicationSystems::explore>(event); }, ready_)) {}
    void Route() { route_(ready_); }
    [[nodiscard]] const std::vector<browser::SystemEvent>& forwarded() const noexcept { return forwarded_; }
    [[nodiscard]] ExploreSystem::event_type& ready() noexcept { return ready_; }
    [[nodiscard]] const browser::SystemEvent& expected() const noexcept { return expected_; }

   private:
    std::vector<browser::SystemEvent> forwarded_;
    ApplicationSystemStorage::EventSink application_events_;
    SystemEventSink<ExploreSystem::event_type> route_;
    ExploreSystem::event_type ready_;
    browser::SystemEvent expected_;
};
TEST_CASE("shell Explore composition preserves explicit budgets above its permitted CPU count") {
    // Constrain only this test's construction thread, so the process remains untouched.
    const auto budgets = std::async(std::launch::async, [] {
                             const auto cpu = mmltk::common::system::allowed_cpu_set().front();
                             mmltk::common::system::set_thread_affinity({cpu});
                             SettingsSystem settings;
                             ApplicationSystemConfiguration configuration{
                                 .base_visual = {.device = 0, .maximum_width = 64, .maximum_height = 64},
                             };
                             const mmltk::frameworks::gpu::DeviceExecution execution{.device = 0, .placement = {.numa_node = 0, .cpus = {cpu}}};
                             std::vector<std::size_t> result;
                             for (const auto requested : {std::size_t{0}, std::size_t{8}, kExploreMaximumParallelism + 1U}) {
                                 configuration.explore_nproc = requested;
                                 auto explore = make_shell_explore_system(settings, configuration, execution);
                                 result.push_back(explore->snapshot().nproc);
                                 explore->Shutdown();
                             }
                             return result;
                         }).get();
    CHECK(budgets == std::vector<std::size_t>{1U, 8U, kExploreMaximumParallelism});
}
TEST_CASE("shell forwards the first ready Explore event unchanged and warms Upscale once") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<ShellWarmProbe>();
    auto warmed = probe->completed.get_future();
    auto upscale = shell_upscale(backend, probe);
    ExploreRouteHarness route(upscale);
    route.Route();
    REQUIRE(warmed.wait_for(2s) == std::future_status::ready);
    REQUIRE(route.forwarded().size() == 1U);
    CHECK(route.forwarded().front() == route.expected());
    route.Route();
    REQUIRE(route.forwarded().size() == 2U);
    CHECK(route.forwarded().back() == route.expected());
    CHECK(probe->calls.load(std::memory_order_acquire) == 1U);
}
// CLEANUP-IGNORE: Atlas-priority routing has its own warm-up preconditions; the existing route and Upscale fixtures are already shared.
TEST_CASE("shell prioritizes visible atlas completion before warming viewer methods") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<ShellWarmProbe>();
    auto warmed = probe->completed.get_future();
    auto upscale = shell_upscale(backend, probe);
    ExploreRouteHarness route(upscale);
    auto& snapshot = std::get<ExploreChanged>(route.ready()).snapshot;
    snapshot.gallery.slots = {true, false};
    route.Route();
    CHECK(probe->calls.load() == 0U);
    snapshot.gallery.slots[1U] = true;
    snapshot.busy = true;
    route.Route();
    CHECK(probe->calls.load() == 0U);
    snapshot.busy = false;
    route.Route();
    REQUIRE(warmed.wait_for(2s) == std::future_status::ready);
    CHECK(probe->calls.load() == 1U);
}
TEST_CASE("shell keeps Explore ready when warm publishes an isolated Upscale failure") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<ShellWarmProbe>();
    probe->fail = true;
    std::atomic<std::size_t> upscale_failures{0U};
    std::promise<void> failure_published;
    std::promise<void> methods_settled;
    auto failed = failure_published.get_future();
    auto settled = methods_settled.get_future();
    auto upscale = shell_upscale(backend, probe, [&upscale_failures, &failure_published, &methods_settled](UpscaleSystem::event_type event) {
        if (std::holds_alternative<UpscaleFailed>(event)) {
            upscale_failures.fetch_add(1U, std::memory_order_acq_rel);
            failure_published.set_value();
        }
        if (std::visit([](const auto& value) { return value.snapshot.methods[2U].warm; }, event)) methods_settled.set_value();
    });
    ExploreRouteHarness route(upscale);
    route.Route();
    REQUIRE(failed.wait_for(2s) == std::future_status::ready);
    REQUIRE(settled.wait_for(2s) == std::future_status::ready);
    REQUIRE(route.forwarded().size() == 1U);
    CHECK(route.forwarded().front() == route.expected());
    const auto* unchanged = std::get_if<ExploreChanged>(&route.ready());
    REQUIRE(unchanged != nullptr);
    CHECK(unchanged->snapshot.ready);
    CHECK(unchanged->snapshot.frame.valid());
    CHECK(upscale_failures.load(std::memory_order_acquire) == 1U);
    failed.get();
    settled.get();
    route.Route();
    CHECK(probe->calls.load(std::memory_order_acquire) == 1U);
}
TEST_CASE("shell routes essential event encoding failure through continuity recovery") {
    auto backend = std::make_shared<FakeImageBackend>();
    auto probe = std::make_shared<ShellWarmProbe>();
    auto upscale = shell_upscale(backend, probe);
    std::vector<browser::SystemEvent> forwarded;
    ApplicationSystemStorage::EventSink events{[&forwarded](browser::SystemEvent event) { forwarded.push_back(std::move(event)); }};
    std::atomic_uint64_t continuity_losses{0U};
    auto route = make_explore_upscale_event_sink(events, upscale, [&continuity_losses] { continuity_losses.fetch_add(1U, std::memory_order_acq_rel); });
    route(ExploreFailed{
        .snapshot = {.revision = 8U},
        .detail = std::string(kVisualFailureByteCapacity + 1U, 'x'),
    });
    CHECK(forwarded.empty());
    CHECK(continuity_losses.load(std::memory_order_acquire) == 1U);
}
}  // namespace
}  // namespace mmltk::controller::shell
