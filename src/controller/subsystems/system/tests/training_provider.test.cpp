#include "src/controller/subsystems/system/tests/application_data_test_support.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/controller/subsystems/train/training_system.h"
#include "src/controller/services/settings_system.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
using namespace mmltk::controller::test_support;
namespace mmltk::controller {
namespace {
[[nodiscard]] contracts::ProviderOffer provider_offer() {
    return {.offer_id = 17,
            .gpu_name = "A100",
            .gpu_count = 4,
            .gpu_ram_gib = 80.0,
            .hourly_price = 1.0,
            .reliability = 0.99,
            .location = "US",
            .family = contracts::ProviderGpuFamily::A100};
}
class FakeTrainingRuntime final : public TrainingRuntime {
   public:
    FakeTrainingRuntime(std::shared_ptr<mmltk::testsupport::StopGate> gate, const bool fail, const bool inconclusive = false)
        : gate_(std::move(gate)), fail_(fail), inconclusive_(inconclusive) {}
    contracts::ComputeTerminal Train(mmltk::backend::models::rfdetr::TrainRequest, const std::stop_token stop,
                                     const std::function<void(const services::TrainProcessProgress&)>& progress) override {
        progress({.progress = fail_ ? contracts::ComputeProgress{.sequence = 0U, .status = std::string(contracts::kComputeStatusCapacity + 1U, 'x')}
                                    : contracts::ComputeProgress{.sequence = 1U, .completed = 1U, .total = 1U, .status = "trained"}});
        if (!gate_->Wait(stop)) return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Cancelled);
        if (fail_)
            return {.outcome = static_cast<contracts::ComputeOperationOutcome>(255U),
                    .output = std::string(contracts::kComputePathCapacity + 1U, 'x'),
                    .detail = {}};
        return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
    }
    contracts::ProviderQueryResult Query(const contracts::ProviderPreferences&, const std::stop_token stop) override {
        if (!gate_->Wait(stop)) return {.outcome = contracts::ProviderQueryOutcome::Cancelled};
        if (fail_)
            return {.outcome = static_cast<contracts::ProviderQueryOutcome>(255U),
                    .offers = std::vector<contracts::ProviderOffer>(contracts::kProviderOfferCapacity + 1U),
                    .detail = std::string(contracts::kProviderDetailCapacity + 1U, 'x')};
        return {.outcome = contracts::ProviderQueryOutcome::Succeeded, .offers = {provider_offer()}};
    }
    contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&, contracts::ProviderOfferIdentity, int,
                                           std::string_view, std::stop_token) override {
        if (inconclusive_) return contracts::provider_effect_inconclusive("provider result is unknown");
        return {.disposition = contracts::ProviderReconciliationDisposition::Applied, .instance_id = 41};
    }
    contracts::ProviderEffectResult Reconcile(const services::VastReconciliationRequest&, std::stop_token) override {
        return {.disposition = contracts::ProviderReconciliationDisposition::Applied, .instance_id = 41};
    }

   private:
    std::shared_ptr<mmltk::testsupport::StopGate> gate_;
    bool fail_ = false;
    bool inconclusive_ = false;
};
[[nodiscard]] TrainingSystem::RuntimeFactory reconstructing_training_runtime(std::shared_ptr<mmltk::testsupport::StopGate>& gate,
                                                                             std::atomic_size_t& constructions) {
    return [&gate, &constructions] {
        const bool fail = constructions++ == 0U;
        return std::make_unique<FakeTrainingRuntime>(gate, fail);
    };
}
struct QueryCancellationProbe final {
    std::promise<void> started;
    std::promise<void> cancellation_observed;
    std::promise<void> release;
    std::shared_future<void> released = release.get_future().share();
};
class BlockingCancellationTrainingRuntime final : public TrainingRuntime {
   public:
    explicit BlockingCancellationTrainingRuntime(std::shared_ptr<QueryCancellationProbe> probe) : probe_(std::move(probe)) {}
    contracts::ComputeTerminal Train(mmltk::backend::models::rfdetr::TrainRequest, std::stop_token,
                                     const std::function<void(const services::TrainProcessProgress&)>&) override {
        return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
    }
    contracts::ProviderQueryResult Query(const contracts::ProviderPreferences&, const std::stop_token stop) override {
        probe_->started.set_value();
        std::mutex mutex;
        std::condition_variable_any condition;
        std::unique_lock lock(mutex);
        static_cast<void>(condition.wait(lock, stop, [] { return false; }));
        probe_->cancellation_observed.set_value();
        probe_->released.wait();
        return {.outcome = contracts::ProviderQueryOutcome::Cancelled};
    }
    contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&, contracts::ProviderOfferIdentity, int,
                                           std::string_view, std::stop_token) override {
        return contracts::provider_effect_not_applied("unused");
    }
    contracts::ProviderEffectResult Reconcile(const services::VastReconciliationRequest&, std::stop_token) override {
        return contracts::provider_effect_not_applied("unused");
    }

   private:
    std::shared_ptr<QueryCancellationProbe> probe_;
};  // CLEANUP-IGNORE: Diagnostic compiler and blocking training runtime are separate typed dependency fakes.
class BlockingRemoteRuntime final : public TrainingRuntime {
   public:
    explicit BlockingRemoteRuntime(std::shared_ptr<mmltk::testsupport::StopGate> remote_gate) : remote_gate_(std::move(remote_gate)) {}
    contracts::ComputeTerminal Train(mmltk::backend::models::rfdetr::TrainRequest, std::stop_token,
                                     const std::function<void(const services::TrainProcessProgress&)>&) override {
        return contracts::make_compute_terminal(contracts::ComputeOperationOutcome::Succeeded);
    }
    contracts::ProviderQueryResult Query(const contracts::ProviderPreferences&, std::stop_token) override {
        return {.outcome = contracts::ProviderQueryOutcome::Succeeded, .offers = {provider_offer()}};
    }
    contracts::ProviderEffectResult Mutate(contracts::ProviderMutation, const contracts::ProviderPreferences&, contracts::ProviderOfferIdentity, int,
                                           std::string_view, const std::stop_token stop) override {
        const bool released = remote_gate_->Wait(stop);
        if (!released || stop.stop_requested()) return contracts::provider_effect_not_applied("remote effect cancelled", true);
        return {.disposition = contracts::ProviderReconciliationDisposition::Applied, .instance_id = 41};
    }
    contracts::ProviderEffectResult Reconcile(const services::VastReconciliationRequest&, std::stop_token) override {
        return {.disposition = contracts::ProviderReconciliationDisposition::Applied, .instance_id = 41};
    }

   private:
    std::shared_ptr<mmltk::testsupport::StopGate> remote_gate_;
};
TEST_CASE("provider result materialization enforces reflected bounds and identity", "[controller][systems][provider-materialization]") {
    services::VastOfferSummary offer;
    offer.offer_id = 7;
    offer.gpu_name = "H100";
    offer.num_gpus = 4;
    offer.gpu_ram = 80.0;
    offer.dph = 1.0;
    offer.reliability = 0.99;
    offer.geolocation = "US";
    std::vector offers{offer};
    const auto valid = services::materialize_provider_query_result(offers);
    REQUIRE(valid.outcome == contracts::ProviderQueryOutcome::Succeeded);
    REQUIRE(valid.offers.size() == 1U);
    CHECK(valid.offers.front().offer_id == 7);
    offers.front().gpu_name.assign(129U, 'x');
    CHECK(services::materialize_provider_query_result(offers).outcome == contracts::ProviderQueryOutcome::Failed);
    offers.front() = offer;
    offers.front().dph = std::numeric_limits<double>::quiet_NaN();
    CHECK(services::materialize_provider_query_result(offers).outcome == contracts::ProviderQueryOutcome::Failed);
    offers = {offer, offer};
    CHECK(services::materialize_provider_query_result(offers).outcome == contracts::ProviderQueryOutcome::Failed);
    offers.assign(contracts::kProviderOfferCapacity + 1U, offer);
    CHECK(services::materialize_provider_query_result(offers).outcome == contracts::ProviderQueryOutcome::Failed);
    CHECK(contracts::normalize_provider_query_result({.outcome = static_cast<contracts::ProviderQueryOutcome>(255U)}).outcome ==
          contracts::ProviderQueryOutcome::Failed);
    CHECK(contracts::normalize_provider_query_result(
              {.outcome = contracts::ProviderQueryOutcome::Succeeded, .offers = {valid.offers.front(), valid.offers.front()}})
              .outcome == contracts::ProviderQueryOutcome::Failed);
}
TEST_CASE("training owns provider offers and remote control with Busy and lazy failure isolation", "[controller][systems][training]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training");
    ApplicationDataFixture fixture{root};
    // CLEANUP-IGNORE: Provider training intentionally starts without the accepted local-model prerequisite used by
    // local training.
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<mmltk::testsupport::StopGate>();
    std::atomic_size_t constructions = 0U;
    TerminalSequence<TrainingSystem::event_type> terminals;
    TrainingSystem training{settings, dataset, model, reconstructing_training_runtime(gate, constructions), [&](TrainingSystem::event_type event) {
                                if (std::holds_alternative<TrainingProgress>(event)) return;
                                terminals.Publish(std::move(event));
                            }};
    const auto admitted_query = training.Query({});
    CHECK_THROWS_AS(training.Query({}), contracts::BusyError);
    CHECK_THROWS_AS(training.Select({.offer_id = 17}), contracts::BusyError);
    gate->Release();
    const auto provider_failure = terminals.First().get();
    REQUIRE(std::holds_alternative<TrainingChanged>(provider_failure));
    CHECK(std::get<TrainingChanged>(provider_failure).snapshot.offers.revision > admitted_query.offers.revision);
    CHECK(std::get<TrainingChanged>(provider_failure).snapshot.offers.outcome == contracts::ProviderQueryOutcome::Failed);
    CHECK_FALSE(std::get<TrainingChanged>(provider_failure).snapshot.offers.detail.empty());
    const auto admitted_successful_query = training.Query({});
    CHECK(admitted_successful_query.offers.outcome == contracts::ProviderQueryOutcome::Idle);
    CHECK(admitted_successful_query.offers.detail.empty());
    CHECK_FALSE(admitted_successful_query.offers.cancellation_requested);
    CHECK(std::holds_alternative<TrainingChanged>(terminals.Second().get()));
    REQUIRE(training.snapshot().offers.offers.size() == 1U);
    CHECK(training.snapshot().offers.revision >= admitted_successful_query.offers.revision);
    const auto before_selection_revision = training.snapshot().offers.revision;
    const auto selected = training.Select({.offer_id = 17});
    CHECK(selected.offers.selected->offer_id == 17);
    CHECK(selected.offers.revision > before_selection_revision);
    static_cast<void>(training.StartRemote({}));
    CHECK(std::holds_alternative<TrainingChanged>(terminals.Third().get()));
    CHECK(training.snapshot().remote.phase == contracts::RemoteSessionPhase::Running);
    CHECK(constructions == 2U);
}
TEST_CASE("local training resets progress and supports failure Stop and reconstruction", "[controller][systems][training][local]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-local-training");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel();
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<mmltk::testsupport::StopGate>();
    // CLEANUP-IGNORE: Local progress accounting and provider offer sequencing use different event invariants.
    std::atomic_size_t constructions = 0U;
    std::atomic_size_t sequence_one_progress = 0U;
    std::promise<void> first_successful_progress;
    auto first_successful_progress_observed = first_successful_progress.get_future();
    TerminalSequence<TrainingSystem::event_type> terminals;
    TrainingSystem training{settings, dataset, model, reconstructing_training_runtime(gate, constructions), [&](TrainingSystem::event_type event) {
                                if (const auto* progress = std::get_if<TrainingProgress>(&event)) {
                                    if (progress->local.progress.sequence == 1U) {
                                        CHECK(progress->local.generation_frontier != 0U);
                                        if (sequence_one_progress.fetch_add(1U) == 0U) first_successful_progress.set_value();
                                    }
                                    return;
                                }
                                if (const auto* changed = std::get_if<TrainingChanged>(&event); changed && changed->snapshot.local.active) return;
                                terminals.Publish(std::move(event));
                            }};  // CLEANUP-IGNORE: Local training and validation start evidence targets independently typed terminal state.
    static_cast<void>(training.Start({}));
    CHECK_THROWS_AS(training.Start({}), contracts::BusyError);
    gate->Release();
    const auto local_failure = terminals.First().get();
    REQUIRE(std::holds_alternative<TrainingChanged>(local_failure));
    CHECK(std::get<TrainingChanged>(local_failure).snapshot.local.terminal.outcome == contracts::ComputeOperationOutcome::Failed);
    CHECK(std::get<TrainingChanged>(local_failure).snapshot.local.generation_frontier == 1U);
    gate = std::make_shared<mmltk::testsupport::StopGate>();
    static_cast<void>(training.Start({}));
    first_successful_progress_observed.get();
    const auto before_rejected_clear = training.snapshot();
    CHECK_THROWS_AS(training.Clear({}), contracts::BusyError);
    CHECK(training.snapshot() == before_rejected_clear);
    // CLEANUP-IGNORE: Training cancellation and restart assert its local terminal, independently of validation metrics.
    static_cast<void>(training.Stop({}));
    CHECK(std::holds_alternative<TrainingChanged>(terminals.Second().get()));
    CHECK(training.snapshot().local.terminal.outcome == contracts::ComputeOperationOutcome::Cancelled);
    gate->Release();
    static_cast<void>(training.Start({}));
    CHECK(std::holds_alternative<TrainingChanged>(terminals.Third().get()));
    CHECK(training.snapshot().local.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    CHECK(sequence_one_progress == 2U);
    CHECK(constructions == 2U);
}
TEST_CASE("remote training rejects duplicate starts and reconciles an inconclusive create", "[controller][systems][training][provider]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-reconcile");
    ApplicationDataFixture fixture{root};
    auto [settings, dataset, model] = fixture.systems();
    auto gate = std::make_shared<mmltk::testsupport::StopGate>();
    gate->Release();
    std::promise<void> query_done;
    std::promise<void> mutation_done;
    std::promise<void> reconciliation_done;
    std::atomic_size_t changed = 0U;
    TrainingSystem training{settings, dataset, model, [gate] { return std::make_unique<FakeTrainingRuntime>(gate, false, true); },
                            [&](TrainingSystem::event_type event) {
                                if (!std::holds_alternative<TrainingChanged>(event)) return;
                                switch (changed++) {
                                    case 0U: query_done.set_value(); break;
                                    case 1U: mutation_done.set_value(); break;
                                    default: reconciliation_done.set_value(); break;
                                }
                            }};
    static_cast<void>(training.Query({}));
    query_done.get_future().wait();
    static_cast<void>(training.Select({.offer_id = 17}));
    static_cast<void>(training.StartRemote({}));
    mutation_done.get_future().wait();
    CHECK(training.snapshot().remote.reconciliation_pending);
    static_cast<void>(training.RetryReconciliation());
    reconciliation_done.get_future().wait();
    CHECK_FALSE(training.snapshot().remote.reconciliation_pending);
    CHECK(training.snapshot().remote.phase == contracts::RemoteSessionPhase::Running);
    CHECK_THROWS_AS(training.StartRemote({}), contracts::InvalidIntentError);
}
TEST_CASE("provider Clear cancels the active query and clears selection state", "[controller][systems][training][provider]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-clear");
    ApplicationDataFixture fixture{root};
    auto [settings, dataset, model] = fixture.systems();
    auto query_gate = std::make_shared<mmltk::testsupport::StopGate>();
    std::promise<void> query_done;
    TrainingSystem training{settings, dataset, model, [query_gate] { return std::make_unique<FakeTrainingRuntime>(query_gate, false); },
                            [&](TrainingSystem::event_type event) {
                                if (std::holds_alternative<TrainingChanged>(event)) query_done.set_value();
                            }};
    static_cast<void>(training.Query({}));
    const auto stopping = training.Clear({});
    CHECK(stopping.activity == TrainingActivity::ProviderQuery);
    CHECK(stopping.offers.cancellation_requested);
    query_done.get_future().wait();
    CHECK(training.snapshot().offers.outcome == contracts::ProviderQueryOutcome::Cancelled);
    CHECK_FALSE(training.snapshot().offers.cancellation_requested);
    CHECK(training.snapshot().offers.offers.empty());
    CHECK_FALSE(training.snapshot().offers.selected);
}
TEST_CASE("provider Clear is admitted once until the query worker settles", "[controller][systems][training][provider]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-clear-once");
    ApplicationDataFixture fixture{root};
    auto [settings, dataset, model] = fixture.systems();
    auto probe = std::make_shared<QueryCancellationProbe>();
    auto started = probe->started.get_future();
    auto cancellation = probe->cancellation_observed.get_future();
    std::promise<void> settled;
    TrainingSystem training{settings, dataset, model, [probe] { return std::make_unique<BlockingCancellationTrainingRuntime>(probe); },
                            [&settled](TrainingSystem::event_type event) {
                                if (std::holds_alternative<TrainingChanged>(event)) settled.set_value();
                            }};
    static_cast<void>(training.Query({}));
    started.wait();
    const auto stopping = training.Clear({});
    REQUIRE(cancellation.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    CHECK(stopping.offers.cancellation_requested);
    CHECK_THROWS_AS(training.Clear({}), contracts::BusyError);
    CHECK(training.snapshot() == stopping);
    probe->release.set_value();
    settled.get_future().wait();
    CHECK_FALSE(training.snapshot().offers.cancellation_requested);
    CHECK(training.snapshot().offers.outcome == contracts::ProviderQueryOutcome::Cancelled);
}
TEST_CASE("local Stop does not cancel provider query or remote effect", "[controller][systems][training][provider]") {
    // CLEANUP-IGNORE: Stop-isolation and Clear-cancellation are distinct provider ownership scenarios.
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-stop-ownership");
    ApplicationDataFixture fixture{root};
    auto [settings, dataset, model] = fixture.systems();
    auto query_gate = std::make_shared<mmltk::testsupport::StopGate>();
    std::promise<void> query_done;
    TrainingSystem query_training{settings, dataset, model, [query_gate] { return std::make_unique<FakeTrainingRuntime>(query_gate, false); },
                                  [&](TrainingSystem::event_type event) {
                                      if (std::holds_alternative<TrainingChanged>(event)) query_done.set_value();
                                  }};
    static_cast<void>(query_training.Query({}));
    static_cast<void>(query_training.Stop({}));
    query_gate->Release();
    query_done.get_future().wait();
    CHECK(query_training.snapshot().offers.outcome == contracts::ProviderQueryOutcome::Succeeded);
    auto remote_gate = std::make_shared<mmltk::testsupport::StopGate>();
    std::promise<void> offers_ready;
    std::promise<void> remote_done;
    std::atomic_size_t changes = 0U;
    TrainingSystem remote_training{settings, dataset, model, [remote_gate] { return std::make_unique<BlockingRemoteRuntime>(remote_gate); },
                                   [&](TrainingSystem::event_type event) {
                                       if (!std::holds_alternative<TrainingChanged>(event)) return;
                                       if (changes++ == 0U)
                                           offers_ready.set_value();
                                       else
                                           remote_done.set_value();
                                   }};
    static_cast<void>(remote_training.Query({}));
    offers_ready.get_future().wait();
    static_cast<void>(remote_training.Select({.offer_id = 17}));
    const auto remote_admitted = remote_training.StartRemote({});
    CHECK(remote_admitted.remote.outcome == contracts::RemoteOperationOutcome::Idle);
    CHECK(remote_admitted.remote.detail.empty());
    static_cast<void>(remote_training.Stop({}));
    remote_gate->Release();
    remote_done.get_future().wait();
    CHECK(remote_training.snapshot().remote.phase == contracts::RemoteSessionPhase::Running);
    CHECK(remote_training.snapshot().remote.revision > remote_admitted.remote.revision);
}
[[nodiscard]] auto release_training_publication_on_exit(TrainingSystem& system, mmltk::testsupport::StopGate& runtime, std::promise<void>& publication) {
    return mmltk::testsupport::ScopedTestCleanup{[&system, &runtime, &publication] {
        mmltk::testsupport::release_test_promise(publication);
        runtime.Release();
        static_cast<void>(system.Stop({}));
    }};
}
TEST_CASE("training completion releases admission before observer publication returns", "[controller][systems][training][settlement]") {
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-settlement");
    ApplicationDataFixture fixture{root};
    fixture.PrepareModel();
    auto [settings, dataset, model] = fixture.systems();
    auto local_gate = std::make_shared<mmltk::testsupport::StopGate>();
    local_gate->Release();
    std::promise<void> local_publishing;
    std::promise<void> release_local_publication;
    const auto release_local = release_local_publication.get_future().share();
    TrainingSystem local{settings, dataset, model, [local_gate] { return std::make_unique<FakeTrainingRuntime>(local_gate, false); },
                         [&](TrainingSystem::event_type event) {
                             const auto* changed = std::get_if<TrainingChanged>(&event);
                             if (!changed || changed->snapshot.local.active) return;
                             local_publishing.set_value();
                             release_local.wait();
                         }};
    auto release_local_on_exit = release_training_publication_on_exit(local, *local_gate, release_local_publication);
    static_cast<void>(local.Start({}));
    auto publishing = local_publishing.get_future();
    REQUIRE(publishing.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    publishing.get();
    CHECK(local.Stop({}).local.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    CHECK(local.snapshot().local.terminal.outcome == contracts::ComputeOperationOutcome::Succeeded);
    release_local_publication.set_value();
    auto query_gate = std::make_shared<mmltk::testsupport::StopGate>();
    query_gate->Release();
    std::promise<TrainingSnapshot> query_publishing;
    std::promise<void> release_query_publication;
    const auto release_query = release_query_publication.get_future().share();
    TrainingSystem query{settings, dataset, model, [query_gate] { return std::make_unique<FakeTrainingRuntime>(query_gate, false); },
                         [&](TrainingSystem::event_type event) {
                             const auto* changed = std::get_if<TrainingChanged>(&event);
                             if (changed == nullptr) return;
                             query_publishing.set_value(changed->snapshot);
                             release_query.wait();
                         }};
    auto release_query_on_exit = release_training_publication_on_exit(query, *query_gate, release_query_publication);
    static_cast<void>(query.Query({}));
    auto query_publication = query_publishing.get_future();
    REQUIRE(query_publication.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    const auto published = query_publication.get();
    static_cast<void>(query.Stop({}));
    const auto cleared = query.Clear({});
    release_query_publication.set_value();
    CHECK(published.offers.outcome == contracts::ProviderQueryOutcome::Succeeded);
    REQUIRE(published.offers.offers.size() == 1U);
    CHECK(cleared.offers.outcome == contracts::ProviderQueryOutcome::Idle);
    CHECK(cleared.offers.offers.empty());
}
TEST_CASE("training admission and matching cancellation do not invert system and worker locks", "[controller][systems][training][admission]") {
    const bool provider_query = GENERATE(false, true);
    CAPTURE(provider_query);
    const auto root = mmltk::testsupport::make_temp_root("ordinary-training-admission");
    ApplicationDataFixture fixture{root};
    // CLEANUP-IGNORE: Admission-race setup selects local model facts; provider cancellation tests above deliberately
    // do not.
    fixture.PrepareModel();
    auto [settings, dataset, model] = fixture.systems();
    auto runtime_gate = std::make_shared<mmltk::testsupport::StopGate>();
    std::promise<void> done;
    TrainingSystem system{settings, dataset, model, [runtime_gate] { return std::make_unique<FakeTrainingRuntime>(runtime_gate, false); },
                          [&](TrainingSystem::event_type event) {
                              if (!std::holds_alternative<TrainingProgress>(event)) done.set_value();
                          }};
    mmltk::testsupport::TestGate admission{provider_query ? "concurrent provider Query and Clear admission" : "concurrent training Start and Stop admission"};
    std::future<TrainingSnapshot> starting;
    std::future<void> cancelling;
    mmltk::testsupport::ScopedTestCleanup release_runtime{[&] {
        admission.Release();
        runtime_gate->Release();
    }};
    starting = std::async(std::launch::async, [&] {
        admission.receipt().ArriveAndWait();
        return provider_query ? system.Query({}) : system.Start({});
    });
    cancelling = std::async(std::launch::async, [&] {
        admission.receipt().ArriveAndWait();
        if (provider_query) {
            try {
                static_cast<void>(system.Clear({}));
            } catch (const contracts::BusyError&) {}
        } else {
            static_cast<void>(system.Stop({}));
        }
    });
    REQUIRE(admission.WaitEntered(std::chrono::seconds{2}, 2U));
    admission.Release();
    const auto start_status = starting.wait_for(std::chrono::seconds{2});
    const auto cancel_status = cancelling.wait_for(std::chrono::seconds{2});
    if (start_status != std::future_status::ready || cancel_status != std::future_status::ready) runtime_gate->Release();
    REQUIRE(start_status == std::future_status::ready);
    REQUIRE(cancel_status == std::future_status::ready);
    CHECK_NOTHROW(static_cast<void>(starting.get()));
    CHECK_NOTHROW(cancelling.get());
    // Clear alone may reject busy provider work at admission. Local Stop must
    // never acquire that exception policy; each generated case settles its own API.
    if (provider_query)
        static_cast<void>(system.Clear({}));
    else
        static_cast<void>(system.Stop({}));
    auto terminal = done.get_future();
    REQUIRE(terminal.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
    terminal.get();
}
}  // namespace
}  // namespace mmltk::controller
