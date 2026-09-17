#include "src/controller/services/tests/support/diagnostics_client_test_access.h"
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/async_test_utils.hpp"
#include "src/common/io/scoped_fd.h"
#include "src/controller/contracts/gui_settings_mutation.h"
#include "src/controller/contracts/workspace.h"
#include "src/controller/services/diagnostics_client.h"
#include "src/controller/services/file_dialog_client.h"
#include "src/controller/services/file_dialog_catalog.h"
#include "src/controller/services/firefox_process_owner.h"
#include "src/controller/services/runtime_diagnostics.h"
#include "src/controller/services/runtime_diagnostic_span.h"
#include "src/controller/services/settings_store.h"
namespace mmltk::controller::services {
namespace {
struct DiagnosticCountingClock final {
    using time_point = std::chrono::steady_clock::time_point;
    static inline std::uint64_t reads = 0U;
    [[nodiscard]] static time_point now() noexcept {
        ++reads;
        return time_point{std::chrono::nanoseconds{static_cast<std::int64_t>(reads)}};
    }
};
using mmltk::common::io::ScopedFd;
[[nodiscard]] std::pair<ScopedFd, ScopedFd> make_diagnostic_pipe(const int flags = 0, const int capacity = 0) {
    int descriptors[2]{-1, -1};
    REQUIRE(::pipe2(descriptors, O_CLOEXEC | flags) == 0);
    std::pair<ScopedFd, ScopedFd> pipe{ScopedFd{descriptors[0]}, ScopedFd{descriptors[1]}};
    if (capacity != 0) REQUIRE(::fcntl(pipe.second.get(), F_SETPIPE_SZ, capacity) > 0);
    return pipe;
}
class ScopedEnvironmentVariable final {
   public:
    explicit ScopedEnvironmentVariable(const std::string_view name) : name_(name) {
        if (const char* existing = std::getenv(name_.c_str()); existing != nullptr) previous_.emplace(existing);
        if (::unsetenv(name_.c_str()) != 0) throw std::runtime_error("cannot unset test environment variable");
    }
    ScopedEnvironmentVariable(const std::string_view name, const std::string_view value) : name_(name) {
        if (const char* existing = std::getenv(name_.c_str()); existing != nullptr) previous_.emplace(existing);
        if (::setenv(name_.c_str(), std::string{value}.c_str(), 1) != 0) throw std::runtime_error("cannot set test environment variable");
    }
    ~ScopedEnvironmentVariable() noexcept {
        if (previous_)
            static_cast<void>(::setenv(name_.c_str(), previous_->c_str(), 1));
        else
            static_cast<void>(::unsetenv(name_.c_str()));
    }

   private:
    std::string name_;
    std::optional<std::string> previous_{};
};
[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("cannot open test output");
    const std::streampos end = stream.tellg();
    if (end < 0) throw std::runtime_error("cannot size test output");
    std::string contents(static_cast<std::size_t>(end), '\0');
    stream.seekg(0);
    if (!contents.empty() && !stream.read(contents.data(), static_cast<std::streamsize>(contents.size()))) {
        throw std::runtime_error("cannot read test output");
    }
    return contents;
}
[[nodiscard]] std::string maximum_diagnostic_record() {
    std::string record{"{\"payload\":\""};
    record.append(DiagnosticsClient::kRecordCapacity - record.size() - 2U, 'x');
    record += "\"}";
    return record;
}
void require_one_terminal_wake(DiagnosticsClient& diagnostics) {
    const int terminal_fd = diagnostics.terminal_fd();
    REQUIRE(terminal_fd >= 0);
    pollfd ready{.fd = terminal_fd, .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&ready, 1U, 5000) == 1);
    std::uint64_t wake = 0U;
    REQUIRE(::read(terminal_fd, &wake, sizeof(wake)) == static_cast<ssize_t>(sizeof(wake)));
    CHECK(wake == 1U);
    CHECK(::read(terminal_fd, &wake, sizeof(wake)) < 0);
    CHECK(errno == EAGAIN);
}
[[nodiscard]] std::filesystem::path make_dialog_helper(const std::filesystem::path& directory, const std::string_view name, const std::string_view body) {
    const std::filesystem::path helper = directory / std::string{name};
    {
        std::ofstream stream(helper);
        stream << "#!/bin/sh\n" << body << "\n";
    }
    std::filesystem::permissions(helper, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write | std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace);
    return helper;
}
[[nodiscard]] FileDialogRequest dialog_request(const std::string_view title = "Select",
                                               const mmltk::controller::contracts::FileDialogMode mode = mmltk::controller::contracts::FileDialogMode::OpenFile,
                                               const std::string_view filter = "Files", const std::string_view pattern = "*") {
    return {.title = mmltk::controller::services::BoundedText<mmltk::controller::services::kFileDialogTextCapacity>::From(title),
            .mode = mode,
            .filter = {.name = mmltk::controller::services::BoundedText<mmltk::controller::services::kFileDialogTextCapacity>::From(filter),
                       .pattern = mmltk::controller::services::BoundedText<mmltk::controller::services::kFileDialogTextCapacity>::From(pattern)}};
}
[[nodiscard]] FileDialogResult run_dialog(const std::filesystem::path& helper, const std::filesystem::path& launch_directory, const FileDialogRequest& request,
                                          const bool cancel_before_run = false) {
    FileDialogClientOwner owner{helper.string(), launch_directory.string()};
    auto [cancellation, token] = FileDialogCancellationSource::Mint();
    if (cancel_before_run) REQUIRE(cancellation.RequestCancel());
    return owner.client().run(request, std::move(token));
}
}  // namespace
TEST_CASE("browser runtime exit policy classifies every owned Firefox terminal", "[gui][services][firefox][lifecycle]") {
    CHECK(browser_runtime_exit_status({.terminal = FirefoxProcessTerminal::Exited, .status = 0}, true) == 0);
    CHECK(browser_runtime_exit_status({.terminal = FirefoxProcessTerminal::Exited, .status = 0}, false) == 1);
    CHECK(browser_runtime_exit_status({.terminal = FirefoxProcessTerminal::Exited, .status = 0, .stop_requested = true}, true) == 0);
    CHECK(browser_runtime_exit_status({.terminal = FirefoxProcessTerminal::Exited, .status = 17, .stop_requested = true}, true) == 17);
    CHECK(browser_runtime_exit_status({.terminal = FirefoxProcessTerminal::Signaled, .status = 128 + SIGTERM, .stop_requested = true, .kill_selected = false},
                                      true) == 0);
    CHECK(browser_runtime_exit_status({.terminal = FirefoxProcessTerminal::Signaled, .status = 128 + SIGTERM}, true) == 128 + SIGTERM);
    CHECK(browser_runtime_exit_status({.terminal = FirefoxProcessTerminal::Signaled, .status = 128 + SIGTERM, .stop_requested = true, .kill_selected = true},
                                      true) == 128 + SIGTERM);
    CHECK(browser_runtime_exit_status({.terminal = FirefoxProcessTerminal::StartupFailed, .status = 1}, true) == 1);
    CHECK(browser_runtime_exit_status({.terminal = FirefoxProcessTerminal::Signaled, .status = 128 + SIGTERM, .stop_requested = true}, false) == 128 + SIGTERM);
}
TEST_CASE("settings store repairs missing malformed and normalized documents", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-settings-store-repair"};
    const auto path = temporary.path() / "gui.json";
    const auto defaults = SettingsStore::load(path.string());
    REQUIRE(defaults.succeeded());
    CHECK(gui_settings_valid(*defaults.settings));
    CHECK(defaults.revision_frontier == 0U);
    CHECK_FALSE(std::filesystem::exists(path));
    {
        std::ofstream malformed(path);
        malformed << "{";
    }
    const auto repaired = SettingsStore::load(path.string());
    REQUIRE(repaired.succeeded());
    CHECK(gui_settings_valid(*repaired.settings));
    CHECK(std::filesystem::exists(path));
    {
        std::ofstream normalizable(path);
        normalizable << R"({"schema_version":0})";
    }
    const auto normalized = SettingsStore::load(path.string());
    REQUIRE(normalized.succeeded());
    CHECK(gui_settings_valid(*normalized.settings));
    CHECK(read_file(path).find("schema_version") != std::string::npos);
}
TEST_CASE("settings store validates, creates parents, and cleans failed atomics", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-settings-store-write"};
    const auto root = temporary.path();
    const auto path = root / "nested" / "gui.json";
    const auto record = SettingsStore::load(path.string());
    REQUIRE(record.succeeded());
    REQUIRE(SettingsStore::save(path.string(), *record.settings, 1U).succeeded());
    CHECK(std::filesystem::exists(path));
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    auto invalid = *record.settings;
    invalid.workflows.train.request.preset_name.clear();
    const auto invalid_save = SettingsStore::save(path.string(), invalid, 2U);
    CHECK_FALSE(invalid_save.succeeded());
    CHECK(invalid_save.stage == SettingsStoreWriteStage::Validation);
    const auto blocked = root / "blocked";
    {
        std::ofstream blocker(blocked);
        blocker << "file";
    }
    const auto blocked_save = SettingsStore::save((blocked / "gui.json").string(), *record.settings, 2U);
    CHECK_FALSE(blocked_save.succeeded());
    CHECK(blocked_save.stage == SettingsStoreWriteStage::Parent);
    CHECK_FALSE(std::filesystem::exists((blocked / "gui.json").string() + ".tmp"));
    const auto rename_target = root / "rename-target";
    std::filesystem::create_directory(rename_target);
    const auto rename_save = SettingsStore::save(rename_target.string(), *record.settings, 2U);
    INFO(rename_save.detail);
    CHECK_FALSE(rename_save.succeeded());
    CHECK(rename_save.stage == SettingsStoreWriteStage::Rename);
    CHECK_FALSE(std::filesystem::exists(rename_target.string() + ".tmp"));
}
TEST_CASE("settings store preserves one durable revision frontier", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-settings-store-revision"};
    const auto path = temporary.path() / "gui.json";
    auto record = SettingsStore::load(path.string());
    record.revision_frontier = 41U;
    REQUIRE(record.succeeded());
    record.settings->ui.dark_mode = true;
    REQUIRE(SettingsStore::save(path.string(), *record.settings, record.revision_frontier).succeeded());
    const auto reloaded = SettingsStore::load(path.string());
    REQUIRE(reloaded.succeeded());
    CHECK(reloaded.revision_frontier == 41U);
    CHECK(reloaded.settings->ui.dark_mode);
    CHECK_FALSE(SettingsStore::save(path.string(), *record.settings, 41U).succeeded());
    {
        std::ofstream invalid(path);
        invalid << R"({"schema_version":1,"settings_revision":-1})";
    }
    const auto invalid_load = SettingsStore::load(path.string());
    CHECK_FALSE(invalid_load.succeeded());
    CHECK_FALSE(invalid_load.settings);
    CHECK(invalid_load.stage == SettingsStoreWriteStage::Validation);
}
TEST_CASE("diagnostics disabled producers perform no submission work", "[gui][services]") {
    DiagnosticsClient diagnostics;
    CHECK_FALSE(diagnostics.enabled());
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Drained);
    CHECK(diagnostics.terminal_fd() < 0);
    const auto producer = diagnostics.producer();
    CHECK_FALSE(producer.enabled());
    CHECK(producer.acquire().submit({"{\"event\":\"disabled\"}"}) == DiagnosticSubmitResult::Disabled);
    CHECK(diagnostics.counters().accepted == 0U);
    RuntimeDiagnostics runtime{diagnostics.producer()};
    const auto target = runtime.target();
    CHECK_FALSE(target.benchmark_trace_enabled());
    CHECK_FALSE(target.pixel_probes_enabled());
    unsigned collections = 0U;
    const auto ids = DiagnosticSpanIds::issued();
    DiagnosticCountingClock::reads = 0U;
    RuntimeDiagnosticSpan<RuntimeDiagnosticTarget, RuntimeDiagnosticFact, DiagnosticCountingClock> span{target, [&] {
                                                                                                            ++collections;
                                                                                                            return std::pair{RuntimeDiagnosticFact{},
                                                                                                                             RuntimeDiagnosticFact{}};
                                                                                                        }};
    span.FinishWith([&](auto&) { ++collections; });
    target.Emit([&] {
        ++collections;
        static_cast<void>(DiagnosticCountingClock::now());
        return RuntimeDiagnosticFact{.event = "disabled.lazy"};
    });
    CHECK(collections == 0U);
    CHECK(DiagnosticCountingClock::reads == 0U);
    CHECK(DiagnosticSpanIds::issued() == ids);
}
TEST_CASE("diagnostic spans pair overlapping intervals and explicit asynchronous parents", "[gui][services]") {
    struct Capture final {
        std::array<RuntimeDiagnosticFact, 4U>* output;
        std::size_t* size;
        [[nodiscard]] bool valid() const noexcept { return true; }
        void operator()(RuntimeDiagnosticFact fact) const noexcept { (*output)[(*size)++] = fact; }
    };
    std::array<RuntimeDiagnosticFact, 4U> captured{};
    std::size_t count = 0U;
    Capture sink{&captured, &count};
    auto facts = [] { return std::pair{RuntimeDiagnosticFact{.event = "begin"}, RuntimeDiagnosticFact{.event = "end"}}; };
    DiagnosticCountingClock::reads = 0U;
    RuntimeDiagnosticSpan<Capture, RuntimeDiagnosticFact, DiagnosticCountingClock> outer{sink, facts};
    const auto parent = outer.link();
    RuntimeDiagnosticSpan<Capture, RuntimeDiagnosticFact, DiagnosticCountingClock> child{sink, facts, parent};
    outer.Finish();
    child.Finish(contracts::DiagnosticSpanOutcome::Cancelled);
    REQUIRE(count == 4U);
    CHECK(captured[0U].context.span.span_outcome == contracts::DiagnosticSpanOutcome::Unspecified);
    CHECK(captured[1U].context.span.span_outcome == contracts::DiagnosticSpanOutcome::Unspecified);
    CHECK(captured[0U].context.link.span_id == captured[2U].context.link.span_id);
    CHECK(captured[1U].context.link.span_id == captured[3U].context.link.span_id);
    CHECK(captured[1U].context.link.span_id != parent.span_id);
    CHECK(captured[1U].context.link.trace_id == parent.trace_id);
    CHECK(captured[1U].context.link.parent_span_id == parent.span_id);
    CHECK(DiagnosticCountingClock::reads == 4U);
}
TEST_CASE("effect-only submission drops on queue-lock contention while complete delivery preserves evidence", "[gui][services]") {
    using namespace mmltk::testsupport;
    ScopedTempDir temporary{"mmltk-diagnostics-contention"};
    DiagnosticsClient diagnostics{temporary.path() / "trace.jsonl"};
    RuntimeDiagnostics complete{diagnostics.producer(), false, RuntimeDiagnosticDelivery::Complete};
    TestGate held{"diagnostic queue lock held"};
    std::jthread holder{[&] {
        auto lock = DiagnosticsClientTestAccess::LockQueue(diagnostics);
        held.receipt().ArriveAndWait();
    }};
    std::future<void> reliable;
    ScopedTestCleanup release{[&] {
        held.Release();
        diagnostics.close(DiagnosticsCloseMode::Discard);
    }};
    REQUIRE(held.WaitEntered(std::chrono::seconds{2}));
    CHECK(diagnostics.producer().acquire().try_submit({"{\"event\":\"lossy\"}"}) == DiagnosticSubmitResult::Contended);
    reliable = std::async(std::launch::async, [target = complete.target()] { target.write({.event = "reliable"}); });
    held.Release();
    await_test_future(reliable, "contended complete submission");
    holder.join();
    diagnostics.close();
    require_one_terminal_wake(diagnostics);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Drained);
    CHECK(diagnostics.counters().accepted == 1U);
    CHECK(diagnostics.counters().dropped == 1U);
}
TEST_CASE("bounded runtime projection escapes valid text and rejects malformed UTF8", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-diagnostics-text"};
    const auto path = temporary.path() / "trace.jsonl";
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
    REQUIRE(descriptor >= 0);
    DiagnosticsClient diagnostics{ScopedFd{descriptor}, DiagnosticsExecutionPolicy::CallerDriven};
    RuntimeDiagnostics runtime{diagnostics.producer()};
    const std::string message{"quoted \"line\"\n\t\xc3\xa9"};
    runtime.write({.event = "text", .message = message});
    runtime.write({.event = "text", .message = "\xc0\x80"});
    runtime.write({.event = "text", .message = "\xed\xa0\x80"});
    CHECK(diagnostics.counters().accepted == 1U);
    diagnostics.close();
    const auto json = nlohmann::json::parse(read_file(path));
    CHECK(json["message"] == message);
    CHECK(json["span_outcome"] == static_cast<std::uint8_t>(contracts::DiagnosticSpanOutcome::Unspecified));
}
TEST_CASE("diagnostic spans preserve typed correlation and explicit scope outcomes", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-diagnostic-spans"};
    const auto path = temporary.path() / "spans.jsonl";
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
    REQUIRE(descriptor >= 0);
    DiagnosticsClient diagnostics{ScopedFd{descriptor}, DiagnosticsExecutionPolicy::CallerDriven};
    RuntimeDiagnostics runtime{diagnostics.producer()};
    const auto target = runtime.target();
    REQUIRE(target.valid());
    CHECK_FALSE(target.pixel_probes_enabled());
    RuntimeDiagnostics probing{diagnostics.producer(), true};
    CHECK(probing.target().pixel_probes_enabled());
    const contracts::DiagnosticContext correlation{
        .surface_high = 11U,
        .surface_low = 12U,
        .selection_generation = 13U,
        .frame_revision = 14U,
        .source = {.source_session = 3U, .source_instance = 1U, .source_revision = 14U, .clean_revision = 10U, .source_observation_revision = 42U},
        .demand = {.demand_generation = 9U},
        .publication = {.presentation_revision = 17U},
        .allocation = {.allocation_generation = 8U},
        .transfer = {.transfer_sequence = 21U, .timeline_ready = 41U},
    };
    const auto facts = [&] {
        RuntimeDiagnosticFact begin{.owner = contracts::DiagnosticOwner::Presentation, .event = "boundary.started", .context = correlation};
        auto end = begin;
        end.event = "boundary.completed";
        return std::pair{begin, end};
    };
    {
        RuntimeDiagnosticSpan span{target, facts};
        span.Finish();
    }
    {
        RuntimeDiagnosticSpan span{target, facts};
        span.Finish(contracts::DiagnosticSpanOutcome::Cancelled);
    }
    try {
        RuntimeDiagnosticSpan span{target, facts};
        throw std::runtime_error("boundary failed");
    } catch (const std::runtime_error&) {}
    { RuntimeDiagnosticSpan span{target, facts}; }
    diagnostics.close();
    const std::array outcomes{contracts::DiagnosticSpanOutcome::Success, contracts::DiagnosticSpanOutcome::Cancelled,
                              contracts::DiagnosticSpanOutcome::Exception, contracts::DiagnosticSpanOutcome::ScopeExit};
    std::ifstream input{path};
    std::string line;
    for (const auto outcome : outcomes) {
        REQUIRE(static_cast<bool>(std::getline(input, line)));
        CHECK(nlohmann::json::parse(line)["event"] == "boundary.started");
        REQUIRE(static_cast<bool>(std::getline(input, line)));
        const auto record = nlohmann::json::parse(line);
        CHECK(record["event"] == "boundary.completed");
        CHECK(record["owner"] == "presentation");
        CHECK(record["source_revision"] == 14U);
        CHECK(record["clean_revision"] == 10U);
        CHECK(record["source_observation_revision"] == 42U);
        CHECK(record["demand_generation"] == 9U);
        CHECK(record["selection_generation"] == 13U);
        CHECK(record["presentation_revision"] == 17U);
        CHECK(record["allocation_generation"] == 8U);
        CHECK(record["transfer_sequence"] == 21U);
        CHECK(record["span_outcome"] == static_cast<std::uint8_t>(outcome));
        CHECK(record["duration_ns"].is_number_unsigned());
    }
    CHECK_FALSE(std::getline(input, line));
    CHECK_FALSE(target.valid());
    CHECK_FALSE(probing.target().pixel_probes_enabled());
}
TEST_CASE("diagnostic span overflow and shutdown lose effects only", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-diagnostic-span-overflow"};
    const int descriptor = ::open((temporary.path() / "trace.jsonl").c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
    REQUIRE(descriptor >= 0);
    DiagnosticsClient diagnostics{ScopedFd{descriptor}, DiagnosticsExecutionPolicy::CallerDriven};
    RuntimeDiagnostics runtime{diagnostics.producer()};
    auto target = runtime.target();
    const auto operation = diagnostics.producer().acquire();
    for (std::size_t index = 0U; index < DiagnosticsClient::kQueueCapacity; ++index)
        REQUIRE(operation.submit({"{\"event\":\"full\"}"}) == DiagnosticSubmitResult::Accepted);
    bool executed = false;
    {
        RuntimeDiagnosticSpan span{target, [] { return std::pair{RuntimeDiagnosticFact{.event = "begin"}, RuntimeDiagnosticFact{.event = "end"}}; }};
        executed = true;
        span.Finish();
    }
    CHECK(executed);
    CHECK(diagnostics.counters().accepted == DiagnosticsClient::kQueueCapacity);
    CHECK(diagnostics.counters().dropped == 2U);
    {
        RuntimeDiagnosticSpan span{target, [] { return std::pair{RuntimeDiagnosticFact{.event = "begin"}, RuntimeDiagnosticFact{.event = "end"}}; }};
        diagnostics.close(DiagnosticsCloseMode::Discard);
    }
    CHECK_FALSE(target.valid());
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Drained);
}
TEST_CASE("runtime trace overflow never waits for a stalled background writer", "[gui][services]") {
    auto [reader, writer] = make_diagnostic_pipe(0, 4096);
    DiagnosticsClient diagnostics{std::move(writer)};
    const auto operation = diagnostics.producer().acquire();
    const std::string record = maximum_diagnostic_record();
    REQUIRE(operation.submit({record}) == DiagnosticSubmitResult::Accepted);
    pollfd readable{.fd = reader.get(), .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&readable, 1U, 5000) == 1);
    RuntimeDiagnostics runtime{diagnostics.producer()};
    for (std::size_t index = 0U; index < DiagnosticsClient::kQueueCapacity + 2U; ++index) runtime.target().write({.event = "capacity", .sequence = index});
    CHECK(diagnostics.counters().dropped >= 2U);
    auto required = std::async(std::launch::async, [target = runtime.target()] { target.write_required({.event = "shutdown.complete"}); });
    const auto required_ready = required.wait_for(std::chrono::seconds{2});
    if (required_ready != std::future_status::ready) diagnostics.close(DiagnosticsCloseMode::Discard);
    REQUIRE(required_ready == std::future_status::ready);
    required.get();
    diagnostics.close(DiagnosticsCloseMode::Discard);
    require_one_terminal_wake(diagnostics);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Drained);
}
TEST_CASE("diagnostics environment uses only the canonical GUI trace path", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-diagnostics-environment"};
    const auto canonical_path = temporary.path() / "gui-trace.jsonl";
    const auto removed_path = temporary.path() / "removed-diagnostics.jsonl";
    ScopedEnvironmentVariable removed{"MMLTK_DIAGNOSTICS_FILE", removed_path.string()};
    {
        ScopedEnvironmentVariable canonical{"MMLTK_GUI_TRACE_FILE", canonical_path.string()};
        auto diagnostics = DiagnosticsClient::from_environment();
        CHECK(diagnostics.enabled());
        CHECK(std::filesystem::exists(canonical_path));
        CHECK_FALSE(std::filesystem::exists(removed_path));
        diagnostics.close(DiagnosticsCloseMode::Discard);
    }
    {
        ScopedEnvironmentVariable canonical{"MMLTK_GUI_TRACE_FILE", ""};
        auto diagnostics = DiagnosticsClient::from_environment();
        CHECK_FALSE(diagnostics.enabled());
        CHECK_FALSE(std::filesystem::exists(removed_path));
    }
    {
        ScopedEnvironmentVariable canonical{"MMLTK_GUI_TRACE_FILE"};
        auto diagnostics = DiagnosticsClient::from_environment();
        CHECK_FALSE(diagnostics.enabled());
        CHECK_FALSE(std::filesystem::exists(removed_path));
    }
}
TEST_CASE("runtime diagnostics owns bounded benchmark trace JSONL", "[gui][services]") {
    int descriptors[2]{-1, -1};
    REQUIRE(::pipe2(descriptors, O_CLOEXEC) == 0);
    ScopedFd reader{descriptors[0]};
    DiagnosticsClient diagnostics{ScopedFd{descriptors[1]}, DiagnosticsExecutionPolicy::CallerDriven};
    RuntimeDiagnostics runtime{diagnostics.producer()};
    const auto target = runtime.target();
    REQUIRE(target.benchmark_trace_enabled());
    target.write_benchmark_trace("benchmark.publication.complete", R"({"output":"/tmp/compiled","train_images":100})");
    target.write_benchmark_trace("benchmark.publication.complete", "{\n\"train_images\":100}");
    target.write_benchmark_trace("benchmark.publication.complete", R"({"train_images":})");
    CHECK(diagnostics.counters().accepted == 1U);
    diagnostics.flush();
    std::array<char, DiagnosticsClient::kRecordCapacity> record{};
    const ssize_t size = ::read(reader.get(), record.data(), record.size());
    REQUIRE(size > 0);
    const std::string_view jsonl{record.data(), static_cast<std::size_t>(size)};
    CHECK(jsonl.contains(R"("kind":"benchmark_dataset")"));
    CHECK(jsonl.contains(R"("name":"benchmark.publication.complete")"));
    CHECK(jsonl.contains(R"("train_images":100)"));
    diagnostics.close(DiagnosticsCloseMode::Discard);
}
TEST_CASE("diagnostics close publishes one synchronous owner terminal for manual clients", "[gui][services]") {
    int descriptors[2]{-1, -1};
    REQUIRE(::pipe2(descriptors, O_CLOEXEC) == 0);
    ScopedFd reader{descriptors[0]};
    DiagnosticsClient diagnostics{ScopedFd{descriptors[1]}, DiagnosticsExecutionPolicy::CallerDriven};
    REQUIRE(diagnostics.producer().acquire().submit({"{\"event\":\"terminal\"}"}) == DiagnosticSubmitResult::Accepted);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Pending);
    diagnostics.close(DiagnosticsCloseMode::Discard);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Drained);
    require_one_terminal_wake(diagnostics);
    CHECK(diagnostics.producer().acquire().submit({"{\"event\":\"closed\"}"}) == DiagnosticSubmitResult::Disabled);
    diagnostics.close(DiagnosticsCloseMode::Flush);
}
TEST_CASE("diagnostics flush close settles a queued manual owner exactly once", "[gui][services]") {
    int descriptors[2]{-1, -1};
    REQUIRE(::pipe2(descriptors, O_CLOEXEC) == 0);
    ScopedFd reader{descriptors[0]};
    DiagnosticsClient diagnostics{ScopedFd{descriptors[1]}, DiagnosticsExecutionPolicy::CallerDriven};
    const auto operation = diagnostics.producer().acquire();
    REQUIRE(operation.submit({"{\"index\":1}"}) == DiagnosticSubmitResult::Accepted);
    REQUIRE(operation.submit({"{\"index\":2}"}) == DiagnosticSubmitResult::Accepted);
    diagnostics.close(DiagnosticsCloseMode::Flush);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Drained);
    CHECK(diagnostics.counters().accepted == 2U);
    CHECK(diagnostics.counters().flushed == 2U);
    CHECK(diagnostics.counters().dropped == 0U);
    require_one_terminal_wake(diagnostics);
    std::array<char, 32U> contents{};
    const ssize_t read = ::read(reader.get(), contents.data(), contents.size());
    REQUIRE(read > 0);
    CHECK(std::string_view{contents.data(), static_cast<std::size_t>(read)} == "{\"index\":1}\n{\"index\":2}\n");
}
TEST_CASE("diagnostics writer failure publishes a failed terminal and releases its owner", "[gui][services]") {
    const int full = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
    REQUIRE(full >= 0);
    DiagnosticsClient diagnostics{ScopedFd{full}, DiagnosticsExecutionPolicy::CallerDriven};
    const auto operation = diagnostics.producer().acquire();
    REQUIRE(operation.submit({"{\"event\":\"fail\"}"}) == DiagnosticSubmitResult::Accepted);
    diagnostics.flush();
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Failed);
    CHECK_FALSE(diagnostics.enabled());
    CHECK(diagnostics.counters().write_failures == 1U);
    CHECK(diagnostics.counters().dropped == 1U);
    CHECK(operation.submit({"{\"event\":\"after-failure\"}"}) == DiagnosticSubmitResult::Disabled);
    require_one_terminal_wake(diagnostics);
    diagnostics.close(DiagnosticsCloseMode::Flush);
}
TEST_CASE("diagnostics manual flush close makes a full descriptor terminal without blocking", "[gui][services]") {
    auto [reader, writer] = make_diagnostic_pipe(O_NONBLOCK);
    std::array<char, 4096U> fill{};
    while (::write(writer.get(), fill.data(), fill.size()) > 0) {}
    REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
    const int owned_writer = writer.get();
    DiagnosticsClient diagnostics{std::move(writer), DiagnosticsExecutionPolicy::CallerDriven};
    REQUIRE(diagnostics.producer().acquire().submit({"{\"event\":\"full\"}"}) == DiagnosticSubmitResult::Accepted);
    diagnostics.close(DiagnosticsCloseMode::Flush);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Failed);
    CHECK(diagnostics.counters().write_failures == 1U);
    CHECK(diagnostics.counters().dropped == 1U);
    CHECK(::fcntl(owned_writer, F_GETFD) == -1);
    CHECK(errno == EBADF);
    require_one_terminal_wake(diagnostics);
    diagnostics.close(DiagnosticsCloseMode::Flush);
    diagnostics.close(DiagnosticsCloseMode::Discard);
    std::uint64_t extra_wake = 0U;
    CHECK(::read(diagnostics.terminal_fd(), &extra_wake, sizeof(extra_wake)) < 0);
    CHECK(errno == EAGAIN);
}
TEST_CASE("diagnostics manual flush publishes one terminal after a partial write", "[gui][services]") {
    auto [reader, writer] = make_diagnostic_pipe(O_NONBLOCK, 4096);
    DiagnosticsClient diagnostics{std::move(writer), DiagnosticsExecutionPolicy::CallerDriven};
    const std::string record = maximum_diagnostic_record();
    REQUIRE(diagnostics.producer().acquire().submit({record}) == DiagnosticSubmitResult::Accepted);
    diagnostics.flush();
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Failed);
    CHECK(diagnostics.counters().write_failures == 1U);
    CHECK(diagnostics.counters().flushed == 0U);
    CHECK(diagnostics.counters().dropped == 1U);
    require_one_terminal_wake(diagnostics);
    diagnostics.close(DiagnosticsCloseMode::Flush);
    std::uint64_t extra_wake = 0U;
    CHECK(::read(diagnostics.terminal_fd(), &extra_wake, sizeof(extra_wake)) < 0);
    CHECK(errno == EAGAIN);
}
TEST_CASE("diagnostics validates records and fixed capacity", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-diagnostics-capacity"};
    const auto path = temporary.path() / "diagnostics.jsonl";
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    REQUIRE(descriptor >= 0);
    DiagnosticsClient diagnostics{ScopedFd{descriptor}, DiagnosticsExecutionPolicy::CallerDriven};
    RuntimeDiagnostics runtime{diagnostics.producer()};
    const auto operation = diagnostics.producer().acquire();
    CHECK(operation.submit({"not-json"}) == DiagnosticSubmitResult::InvalidJson);
    CHECK(operation.submit({"[]"}) == DiagnosticSubmitResult::InvalidJson);
    CHECK(operation.submit({"{\"bad\":\n1}"}) == DiagnosticSubmitResult::InvalidJson);
    CHECK(operation.submit({" \t{\"valid\":true}\t "}) == DiagnosticSubmitResult::Accepted);
    std::string oversized(DiagnosticsClient::kRecordCapacity + 1U, 'x');
    CHECK(operation.submit({oversized}) == DiagnosticSubmitResult::RecordTooLarge);
    for (std::size_t index = 1U; index < DiagnosticsClient::kQueueCapacity; ++index) {
        CHECK(operation.submit({"{\"index\":1}"}) == DiagnosticSubmitResult::Accepted);
    }
    CHECK(operation.submit({"{\"index\":2}"}) == DiagnosticSubmitResult::Capacity);
    runtime.target().write_required({.event = "shutdown.complete"});
    CHECK(diagnostics.counters().accepted == DiagnosticsClient::kQueueCapacity + 1U);
    CHECK(operation.submit({"{\"index\":3}"}) == DiagnosticSubmitResult::Disabled);
    diagnostics.close(DiagnosticsCloseMode::Flush);
    CHECK(diagnostics.counters().flushed == DiagnosticsClient::kQueueCapacity + 1U);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Drained);
    require_one_terminal_wake(diagnostics);
    std::ifstream input{path};
    std::string last;
    for (std::string line; std::getline(input, line);) last = std::move(line);
    CHECK(last.contains(R"("event":"shutdown.complete")"));
}
TEST_CASE("diagnostics close interrupts a stalled output descriptor", "[gui][services]") {
    for (const DiagnosticsCloseMode mode : {DiagnosticsCloseMode::Discard, DiagnosticsCloseMode::Flush}) {
        INFO((mode == DiagnosticsCloseMode::Discard ? "discard" : "flush"));
        int descriptors[2]{-1, -1};
        REQUIRE(::pipe2(descriptors, O_CLOEXEC) == 0);
        ScopedFd reader{descriptors[0]};
        ScopedFd writer{descriptors[1]};
        const std::string maximum_record = maximum_diagnostic_record();
        const int pipe_capacity = ::fcntl(writer.get(), F_SETPIPE_SZ, 4096);
        REQUIRE(pipe_capacity > 0);
        REQUIRE(static_cast<std::size_t>(pipe_capacity) < maximum_record.size());
        DiagnosticsClient diagnostics{std::move(writer)};
        REQUIRE(diagnostics.producer().acquire().submit({maximum_record}) == DiagnosticSubmitResult::Accepted);
        pollfd readable{.fd = reader.get(), .events = POLLIN, .revents = 0};
        REQUIRE(::poll(&readable, 1U, 5000) == 1);
        diagnostics.close(mode);
        require_one_terminal_wake(diagnostics);
        CHECK(diagnostics.counters().flushed == 0U);
        CHECK(diagnostics.counters().dropped == 1U);
        CHECK(diagnostics.counters().write_failures == (mode == DiagnosticsCloseMode::Flush ? 1U : 0U));
        CHECK(diagnostics.terminal() == (mode == DiagnosticsCloseMode::Flush ? DiagnosticsTerminal::Failed : DiagnosticsTerminal::Drained));
    }
}
TEST_CASE("diagnostics flush preserves order and reports write failure", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-diagnostics-order"};
    const auto path = temporary.path() / "diagnostics.jsonl";
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    REQUIRE(descriptor >= 0);
    DiagnosticsClient diagnostics{ScopedFd{descriptor}};
    const auto operation = diagnostics.producer().acquire();
    REQUIRE(operation.submit({"{\"index\":1}"}) == DiagnosticSubmitResult::Accepted);
    REQUIRE(operation.submit({"{\"index\":2}"}) == DiagnosticSubmitResult::Accepted);
    diagnostics.flush();
    CHECK(read_file(path) == "{\"index\":1}\n{\"index\":2}\n");
    diagnostics.close();
    CHECK(operation.submit({"{\"index\":3}"}) == DiagnosticSubmitResult::Disabled);
    const int full = ::open("/dev/full", O_WRONLY | O_CLOEXEC);
    REQUIRE(full >= 0);
    DiagnosticsClient failing{ScopedFd{full}};
    REQUIRE(failing.producer().acquire().submit({"{\"event\":\"fail\"}"}) == DiagnosticSubmitResult::Accepted);
    failing.flush();
    CHECK(failing.counters().write_failures == 1U);
    CHECK_FALSE(failing.enabled());
    failing.close();
    CHECK(failing.terminal() == DiagnosticsTerminal::Failed);
    require_one_terminal_wake(failing);
}
TEST_CASE("complete background diagnostics preserve unique concurrent single and batch identities", "[gui][services]") {
    using namespace mmltk::testsupport;
    constexpr std::size_t kProducerCount = 4U;
    constexpr std::size_t kRecordsPerProducer = 512U;
    constexpr std::size_t kExpectedRecords = kProducerCount * kRecordsPerProducer;
    ScopedTempDir temporary{"mmltk-diagnostics-lossless"};
    const auto path = temporary.path() / "diagnostics.jsonl";
    DiagnosticsClient diagnostics{path};
    RuntimeDiagnostics runtime{diagnostics.producer(), false, RuntimeDiagnosticDelivery::Complete};
    std::array<std::future<void>, kProducerCount> submitters;
    ScopedTestCleanup close{[&] { diagnostics.close(DiagnosticsCloseMode::Discard); }};
    for (std::size_t producer = 0U; producer != kProducerCount; ++producer) {
        submitters[producer] = std::async(std::launch::async, [target = runtime.target(), producer] {
            for (std::size_t index = 0U; index != kRecordsPerProducer;) {
                if (index % 3U == 0U && index + 1U < kRecordsPerProducer) {
                    const std::array batch{RuntimeDiagnosticFact{.event = "concurrent", .sequence = index, .value = producer},
                                           RuntimeDiagnosticFact{.event = "concurrent", .sequence = index + 1U, .value = producer}};
                    target.write_batch(batch);
                    index += 2U;
                } else {
                    target.write({.event = "concurrent", .sequence = index++, .value = producer});
                }
            }
        });
    }
    for (auto& submitter : submitters) await_test_future(submitter, "complete diagnostic producer", std::chrono::seconds{5});
    runtime.target().write_required({.event = "shutdown.complete"});
    diagnostics.close();
    require_one_terminal_wake(diagnostics);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Drained);
    const auto counters = diagnostics.counters();
    CHECK(counters.accepted == kExpectedRecords + 1U);
    CHECK(counters.flushed == kExpectedRecords + 1U);
    CHECK(counters.dropped == 0U);
    CHECK(counters.write_failures == 0U);
    std::array<std::size_t, kProducerCount> next{};
    std::ifstream input{path};
    std::string line;
    std::size_t terminal = 0U;
    while (std::getline(input, line)) {
        const auto record = nlohmann::json::parse(line);
        if (record.at("event") == "shutdown.complete") {
            ++terminal;
            for (const auto count : next) CHECK(count == kRecordsPerProducer);
        } else {
            CHECK(terminal == 0U);
            const auto producer = record.at("value").get<std::size_t>();
            REQUIRE(producer < next.size());
            CHECK(record.at("sequence") == next[producer]++);
        }
    }
    CHECK(terminal == 1U);
    for (const auto count : next) CHECK(count == kRecordsPerProducer);
}
TEST_CASE("complete diagnostic records and batches wait atomically and settle on drain closure or writer loss", "[gui][services]") {
    using namespace mmltk::testsupport;
    const int outcome = GENERATE(0, 1, 2);
    const bool batch = GENERATE(false, true);
    const std::size_t record_count = batch ? 2U : 1U;
    auto [reader, writer] = make_diagnostic_pipe(0, 4096);
    DiagnosticsClient diagnostics{std::move(writer)};
    RuntimeDiagnostics runtime{diagnostics.producer(), false, RuntimeDiagnosticDelivery::Complete};
    const auto operation = diagnostics.producer().acquire();
    const auto large = maximum_diagnostic_record();
    REQUIRE(operation.submit({large}) == DiagnosticSubmitResult::Accepted);
    pollfd readable{.fd = reader.get(), .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&readable, 1U, 2000) == 1);
    for (std::size_t index = 1U; index < DiagnosticsClient::kQueueCapacity - (record_count - 1U); ++index)
        REQUIRE(operation.submit({"{\"event\":\"filler\"}"}) == DiagnosticSubmitResult::Accepted);
    const auto before = diagnostics.counters().accepted;
    std::future<std::string> output;
    auto submission = std::async(std::launch::async, [&diagnostics, target = runtime.target(), batch, outcome] {
        const std::array facts{RuntimeDiagnosticFact{.event = "batch", .sequence = 1U}, RuntimeDiagnosticFact{.event = "batch", .sequence = 2U}};
        if (batch)
            target.write_batch(facts);
        else
            target.write(facts.front());
        // Submission establishes queue admission. Flush establishes delivery
        // while the reader drains; close remains nonblocking on a stalled pipe.
        if (outcome == 0) diagnostics.flush();
    });
    ScopedTestCleanup close{[&] { diagnostics.close(DiagnosticsCloseMode::Discard); }};
    REQUIRE(DiagnosticsClientTestAccess::WaitForCapacityWaiter(diagnostics));
    CHECK(diagnostics.counters().accepted == before);
    if (outcome == 0) {
        output = std::async(std::launch::async, [&] {
            std::string bytes;
            std::array<char, 4096U> buffer{};
            for (;;) {
                const auto size = ::read(reader.get(), buffer.data(), buffer.size());
                if (size < 0 && errno == EINTR) continue;
                if (size == 0) break;
                if (size < 0) throw std::runtime_error("diagnostic read failed");
                bytes.append(buffer.data(), static_cast<std::size_t>(size));
            }
            return bytes;
        });
    } else if (outcome == 1)
        diagnostics.close(DiagnosticsCloseMode::Discard);
    else
        reader.reset();
    await_test_future(submission, "complete batch settlement");
    diagnostics.close();
    require_one_terminal_wake(diagnostics);
    CHECK(diagnostics.terminal() == (outcome == 0 ? DiagnosticsTerminal::Drained : DiagnosticsTerminal::Failed));
    CHECK(diagnostics.counters().accepted == before + (outcome == 0 ? record_count : 0U));
    if (outcome == 0) {
        const auto bytes = await_test_future(output, "diagnostic reader settlement");
        std::istringstream lines{bytes};
        std::string line;
        std::size_t next = 1U;
        while (std::getline(lines, line)) {
            const auto record = nlohmann::json::parse(line);
            if (record.value("event", "") == "batch") CHECK(record.at("sequence") == next++);
        }
        CHECK(next == record_count + 1U);
    }
}
TEST_CASE("complete lazy factories reserve delivery before closure or terminal sealing", "[gui][services]") {
    using namespace mmltk::testsupport;
    const bool seal = GENERATE(false, true);
    const bool throwing = GENERATE(false, true);
    ScopedTempDir temporary{"mmltk-diagnostics-lazy-close"};
    DiagnosticsClient diagnostics{temporary.path() / "trace.jsonl"};
    RuntimeDiagnostics runtime{diagnostics.producer(), false, RuntimeDiagnosticDelivery::Complete};
    TestGate factory{"complete lazy factory entered"};
    auto emission = std::async(std::launch::async, [target = runtime.target(), receipt = factory.receipt(), throwing] {
        target.Emit([&]() -> RuntimeDiagnosticFact {
            receipt.ArriveAndWait();
            if (throwing) throw std::runtime_error("lazy factory failed after close");
            return {.event = "lazy.fact"};
        });
    });
    ScopedTestCleanup cleanup{[&] {
        factory.Release();
        diagnostics.close(DiagnosticsCloseMode::Discard);
    }};
    REQUIRE(factory.WaitEntered(std::chrono::seconds{2}));
    if (seal) runtime.target().write_required({.event = "shutdown.complete"});
    diagnostics.close(DiagnosticsCloseMode::Flush);
    require_one_terminal_wake(diagnostics);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Failed);
    factory.Release();
    await_test_future(emission, "lazy factory settlement after close");
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Failed);
    CHECK(diagnostics.counters().accepted == (seal ? 1U : 0U));
    CHECK_FALSE(read_file(temporary.path() / "trace.jsonl").contains("lazy.fact"));
}
TEST_CASE("complete diagnostics reject unavailable sinks and encoding or impossible batch loss", "[gui][services]") {
    DiagnosticsClient disabled;
    CHECK_THROWS_AS((RuntimeDiagnostics{disabled.producer(), false, RuntimeDiagnosticDelivery::Complete}), std::runtime_error);
    const int manual_fd = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
    REQUIRE(manual_fd >= 0);
    DiagnosticsClient manual{ScopedFd{manual_fd}, DiagnosticsExecutionPolicy::CallerDriven};
    CHECK_THROWS_AS((RuntimeDiagnostics{manual.producer(), false, RuntimeDiagnosticDelivery::Complete}), std::runtime_error);
    const int failure = GENERATE(0, 1, 2, 3, 4);
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-diagnostics-rejection"};
    DiagnosticsClient diagnostics{temporary.path() / "trace.jsonl"};
    RuntimeDiagnostics runtime{diagnostics.producer(), false, RuntimeDiagnosticDelivery::Complete};
    const auto target = runtime.target();
    if (failure == 0)
        target.write({.event = "invalid event"});
    else if (failure == 1)
        target.write({.event = "invalid", .message = "\xc0\x80"});
    else if (failure == 2) {
        std::array<RuntimeDiagnosticFact, DiagnosticsClient::kQueueCapacity + 1U> facts{};
        for (auto& fact : facts) fact.event = "too_many";
        target.write_batch(facts);
    } else if (failure == 3) {
        const std::array facts{RuntimeDiagnosticFact{.event = "valid"}, RuntimeDiagnosticFact{.event = "invalid", .message = "\xc0\x80"}};
        target.write_batch(facts);
    } else
        target.Emit([]() -> RuntimeDiagnosticFact { throw std::runtime_error("factory failure"); });
    require_one_terminal_wake(diagnostics);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Failed);
    CHECK(diagnostics.counters().accepted == 0U);
    CHECK_FALSE(target.valid());
}
TEST_CASE("diagnostics path sessions truncate and producers expire after close", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-diagnostics-session"};
    const auto path = temporary.path() / "diagnostics.jsonl";
    {
        std::ofstream old(path);
        old << "old\n";
    }
    DiagnosticsProducer producer;
    {
        DiagnosticsClient diagnostics{path};
        producer = diagnostics.producer();
        REQUIRE(producer.acquire().submit({"{\"event\":\"fresh\"}"}) == DiagnosticSubmitResult::Accepted);
        diagnostics.flush();
        CHECK(read_file(path) == "{\"event\":\"fresh\"}\n");
        // Destruction is the final fallback owner cleanup path after an
        // ordinary producer has escaped the client scope.
    }
    CHECK(producer.acquire().submit({"{\"event\":\"closed\"}"}) == DiagnosticSubmitResult::Disabled);
}
TEST_CASE("diagnostics moves release replaced owners without terminal waits", "[gui][services]") {
    int first_descriptors[2]{-1, -1};
    int second_descriptors[2]{-1, -1};
    REQUIRE(::pipe2(first_descriptors, O_CLOEXEC) == 0);
    REQUIRE(::pipe2(second_descriptors, O_CLOEXEC) == 0);
    ScopedFd first_reader{first_descriptors[0]};
    ScopedFd second_reader{second_descriptors[0]};
    DiagnosticsClient first{ScopedFd{first_descriptors[1]}, DiagnosticsExecutionPolicy::CallerDriven};
    REQUIRE(first.producer().acquire().submit({"{\"event\":\"first\"}"}) == DiagnosticSubmitResult::Accepted);
    DiagnosticsClient moved{std::move(first)};
    CHECK(first.terminal() == DiagnosticsTerminal::Drained);
    DiagnosticsClient replacement{ScopedFd{second_descriptors[1]}, DiagnosticsExecutionPolicy::CallerDriven};
    REQUIRE(replacement.producer().acquire().submit({"{\"event\":\"second\"}"}) == DiagnosticSubmitResult::Accepted);
    replacement = std::move(moved);
    CHECK(moved.terminal() == DiagnosticsTerminal::Drained);
    replacement.close(DiagnosticsCloseMode::Discard);
    CHECK(replacement.terminal() == DiagnosticsTerminal::Drained);
    require_one_terminal_wake(replacement);
}
TEST_CASE("diagnostics destruction releases an active detached writer after its terminal", "[gui][services]") {
    auto [reader, writer] = make_diagnostic_pipe();
    const std::string record = maximum_diagnostic_record();
    REQUIRE(::fcntl(writer.get(), F_SETPIPE_SZ, 4096) > 0);
    DiagnosticsProducer::Operation retained;
    ScopedFd terminal;
    {
        DiagnosticsClient diagnostics{std::move(writer)};
        retained = diagnostics.producer().acquire();
        REQUIRE(retained.submit({record}) == DiagnosticSubmitResult::Accepted);
        terminal.reset(::dup(diagnostics.terminal_fd()));
        REQUIRE(terminal.get() >= 0);
        pollfd readable{.fd = reader.get(), .events = POLLIN, .revents = 0};
        REQUIRE(::poll(&readable, 1U, 5000) == 1);
    }
    pollfd terminal_ready{.fd = terminal.get(), .events = POLLIN, .revents = 0};
    REQUIRE(::poll(&terminal_ready, 1U, 5000) == 1);
    std::uint64_t wake = 0U;
    REQUIRE(::read(terminal.get(), &wake, sizeof(wake)) == static_cast<ssize_t>(sizeof(wake)));
    CHECK(wake == 1U);
    CHECK(retained.submit({"{\"event\":\"after-destruction\"}"}) == DiagnosticSubmitResult::Disabled);
}
TEST_CASE("diagnostics close races safely with weak producer submissions", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-diagnostics-concurrent"};
    const auto path = temporary.path() / "diagnostics.jsonl";
    DiagnosticsClient diagnostics{path};
    const auto producer = diagnostics.producer();
    std::thread submitter([operation = producer.acquire()] {
        for (std::size_t index = 0; index < 1024U; ++index) { static_cast<void>(operation.submit({"{\"event\":\"concurrent\"}"})); }
    });
    diagnostics.close(DiagnosticsCloseMode::Discard);
    submitter.join();
    CHECK_FALSE(producer.enabled());
    require_one_terminal_wake(diagnostics);
    CHECK(diagnostics.terminal() == DiagnosticsTerminal::Drained);
}
TEST_CASE("file dialog client has typed pre-cancel and terminal outcomes", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-file-dialog-terminals"};
    const auto selected = make_dialog_helper(temporary.path(), "selected", "printf '%s\\n' chosen.txt");
    const auto pre_cancel_marker = temporary.path() / "pre-cancelled";
    const auto pre_cancel_helper = make_dialog_helper(temporary.path(), "pre-cancel-helper", "printf launched > '" + pre_cancel_marker.string() + "'");
    const auto cancelled = make_dialog_helper(temporary.path(), "cancelled", "exit 1");
    const auto failing = make_dialog_helper(temporary.path(), "failing", "exit 42");
    const FileDialogRequest request = dialog_request();
    CHECK(run_dialog(pre_cancel_helper, temporary.path(), request, true).disposition == FileDialogDisposition::Reaped);
    CHECK_FALSE(std::filesystem::exists(pre_cancel_marker));
    const auto selected_result = run_dialog(selected, temporary.path(), request);
    CHECK(selected_result.disposition == FileDialogDisposition::Selected);
    CHECK(selected_result.path.view() == (temporary.path() / "chosen.txt").string());
    CHECK(run_dialog(cancelled, temporary.path(), request).disposition == FileDialogDisposition::Cancelled);
    const auto process_exit = run_dialog(failing, temporary.path(), request);
    CHECK(process_exit.disposition == FileDialogDisposition::Failed);
    CHECK(process_exit.failure == FileDialogFailure::ProcessExit);
    const auto missing = run_dialog(temporary.path() / "missing", temporary.path(), request);
    CHECK(missing.disposition == FileDialogDisposition::Failed);
    CHECK(missing.failure == FileDialogFailure::CapabilityUnavailable);
}
TEST_CASE("file dialog capability is validated once and later exec failure is typed", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-file-dialog-capability"};
    const auto non_executable = temporary.path() / "non-executable";
    {
        std::ofstream output(non_executable);
        output << "#!/bin/sh\nexit 0\n";
    }
    REQUIRE(::chmod(non_executable.c_str(), 0600) == 0);
    FileDialogClientOwner unavailable{non_executable.string(), temporary.path().string()};
    CHECK_FALSE(unavailable.client().valid());
    const auto removed_after_admission = make_dialog_helper(temporary.path(), "removed-after-admission", "exit 0");
    FileDialogClientOwner admitted{removed_after_admission.string(), temporary.path().string()};
    REQUIRE(admitted.client().valid());
    REQUIRE(::unlink(removed_after_admission.c_str()) == 0);
    auto [cancellation, token] = FileDialogCancellationSource::Mint();
    const auto exec_failure = admitted.client().run(dialog_request(), std::move(token));
    CHECK(exec_failure.disposition == FileDialogDisposition::Failed);
    CHECK(exec_failure.failure == FileDialogFailure::Exec);
}
TEST_CASE("file dialog PATH resolution skips invalid shadow candidates", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-file-dialog-path"};
    const auto shadow = temporary.path() / "shadow";
    const auto valid = temporary.path() / "valid";
    REQUIRE(std::filesystem::create_directories(shadow / "dialog-helper"));
    REQUIRE(std::filesystem::create_directory(valid));
    const auto executable = make_dialog_helper(valid, "dialog-helper", "printf '%s\\n' selected.bin");
    const std::string path = shadow.string() + ":" + valid.string();
    ScopedEnvironmentVariable environment{"PATH", path};
    FileDialogClientOwner owner{"dialog-helper", temporary.path().string()};
    REQUIRE(owner.client().valid());
    auto [cancellation, token] = FileDialogCancellationSource::Mint();
    const auto result = owner.client().run(dialog_request(), std::move(token));
    CHECK(result.disposition == FileDialogDisposition::Selected);
    CHECK(result.path.view() == (temporary.path() / "selected.bin").string());
    CHECK(executable.filename() == "dialog-helper");
}
TEST_CASE("file dialog cancellation source publishes exactly once across moves", "[gui][services]") {
    static_assert(!std::is_copy_constructible_v<FileDialogCancellationSource>);
    static_assert(!std::is_copy_constructible_v<FileDialogCancellationToken>);
    static_assert(std::is_nothrow_move_constructible_v<FileDialogCancellationSource>);
    static_assert(std::is_nothrow_move_constructible_v<FileDialogCancellationToken>);
    auto [source, token] = FileDialogCancellationSource::Mint();
    CHECK(source.valid());
    CHECK(token.valid());
    FileDialogCancellationSource moved_source{std::move(source)};
    CHECK_FALSE(source.valid());
    CHECK_FALSE(source.RequestCancel());
    CHECK(moved_source.RequestCancel());
    CHECK_FALSE(moved_source.RequestCancel());
    FileDialogCancellationToken moved_token{std::move(token)};
    CHECK_FALSE(token.valid());
    CHECK(moved_token.valid());
    FileDialogClient stale;
    {
        FileDialogClientOwner owner{"/bin/true", "/tmp"};
        stale = owner.client();
        CHECK(stale.valid());
    }
    CHECK(stale.valid());
    auto [stale_source, stale_token] = FileDialogCancellationSource::Mint();
    CHECK(stale_source.valid());
    CHECK(stale_source.RequestCancel());
    CHECK(stale.run(dialog_request(), std::move(stale_token)).disposition == FileDialogDisposition::Reaped);
}
TEST_CASE("file dialog owner and value capacities reject before publication", "[gui][services]") {
    const std::string oversized_path(mmltk::controller::services::kFileDialogPathStorageCapacity, 'x');
    const std::string oversized_policy(mmltk::controller::services::kFileDialogTextCapacity, 'x');
    CHECK_FALSE(mmltk::controller::services::BoundedText<mmltk::controller::services::kFileDialogPathStorageCapacity>::From(oversized_path).valid());
    CHECK_FALSE(mmltk::controller::services::BoundedText<mmltk::controller::services::kFileDialogTextCapacity>::From(oversized_policy).valid());
    CHECK_THROWS_AS(FileDialogClientOwner(oversized_path, "/tmp"), std::invalid_argument);
    std::array<std::optional<FileDialogClientOwner>, 8U> owners;
    for (auto& owner : owners) owner.emplace("/bin/true", "/tmp");
    owners[0].reset();
    CHECK_NOTHROW(FileDialogClientOwner("/bin/true", "/tmp"));
}
TEST_CASE("file dialog selected results require an owned bounded nonempty path", "[gui][services]") {
    using mmltk::controller::services::FileDialogSelected;
    using mmltk::controller::services::FileDialogSelection;
    using mmltk::controller::services::FileDialogTarget;
    using mmltk::controller::services::SettingsFieldTarget;
    constexpr std::uint64_t field_id = 7U;
    const FileDialogTarget target{SettingsFieldTarget{field_id}};
    const FileDialogSelection empty{.target = target, .result = FileDialogSelected{""}};
    const FileDialogSelection selected{.target = target, .result = FileDialogSelected{"/tmp/selected"}};
    const FileDialogSelection oversized{.target = target,
                                        .result = FileDialogSelected{std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1U, 'x')}};
    CHECK_FALSE(empty.valid_for(target));
    CHECK(selected.valid_for(target));
    CHECK_FALSE(oversized.valid_for(target));
}
TEST_CASE("workflow path dialogs are projected from controller member paths", "[gui][services][dialogs]") {
    const auto entries = mmltk::controller::services::file_dialog_catalog().entries();
    for (const auto path : {"workflows.validate.request.compiled_path", "workflows.train.request.output_dir", "workflows.train.request.resume_path",
                            "workflows.predict.source.compiled_path", "workflows.predict.source.single_image_path"}) {
        const auto found = std::ranges::find_if(entries, [path](const auto& entry) { return entry.field_path.view() == path; });
        REQUIRE(found != entries.end());
        CHECK_FALSE(found->model_input.has_value());
        CHECK(mmltk::controller::services::resolve_file_dialog_path(found->stable_id, "/chosen/input").has_value());
    }
}
TEST_CASE("model file dialog targets remain typed through native resolution", "[gui][services][model]") {
    using mmltk::backend::models::catalog::ModelArtifactInputKind;
    using mmltk::controller::contracts::FeatureId;
    using mmltk::controller::services::FileDialogOpen;
    using mmltk::controller::services::ModelArtifactTarget;
    const auto entries = mmltk::controller::services::file_dialog_catalog().entries();
    const auto descriptor = std::ranges::find(entries, std::string_view{"workflows.train.request.weights_path"},
                                              [](const mmltk::controller::services::FileDialogDescriptor& value) { return value.field_path.view(); });
    REQUIRE(descriptor != entries.end());
    const auto stable_id = descriptor->stable_id;
    const FileDialogOpen request{
        .target = mmltk::controller::services::FileDialogTarget{ModelArtifactTarget{
            .stable_id = stable_id,
            .workflow = FeatureId::Train,
            .input = ModelArtifactInputKind::Weights,
        }},
    };
    const auto resolved = mmltk::controller::services::file_dialog_catalog().resolve(request);
    REQUIRE(resolved);
    CHECK(resolved->target == request.target);
    CHECK(resolved->descriptor.defer_apply());
    CHECK_FALSE(mmltk::controller::services::file_dialog_catalog().resolve(
        FileDialogOpen{.target = mmltk::controller::services::FileDialogTarget{mmltk::controller::services::SettingsFieldTarget{stable_id}}}));
    auto mismatched = request;
    std::get<ModelArtifactTarget>(mismatched.target.value).input = ModelArtifactInputKind::Onnx;
    CHECK_FALSE(mmltk::controller::services::file_dialog_catalog().resolve(mismatched));
}
TEST_CASE("workspace path values reject malformed boundaries and preserve exact identity", "[gui][services]") {
    using namespace mmltk::controller::contracts;
    constexpr std::string_view resource_name{"image.png"};
    const WorkspaceResource resource = WorkspaceResource::From(resource_name, 7U);
    CHECK(resource.valid());
    CHECK(resource.view() == resource_name);
    CHECK_FALSE(WorkspaceResource::From({}, 7U).valid());
    CHECK_FALSE(WorkspaceResource::From(resource_name, 0U).valid());
    CHECK_FALSE(WorkspaceResource::From(std::string(kWorkspaceResourceCapacity + 1U, 'x'), 7U).valid());
    CHECK_FALSE(WorkspaceResource::From(std::string_view{"bad\0resource", 12U}, 7U).valid());
    WorkspaceResource noncanonical_resource = resource;
    noncanonical_resource.storage.back() = 'x';
    CHECK_FALSE(noncanonical_resource.valid());
    const WorkspacePath path = WorkspacePath::From("/workspace/image.png", 7U);
    CHECK(path.valid());
    CHECK(path.view() == "/workspace/image.png");
    CHECK_FALSE(WorkspacePath::From({}, 7U).valid());
    CHECK_FALSE(WorkspacePath::From("/workspace/image.png", 0U).valid());
    CHECK_FALSE(WorkspacePath::From(std::string(kWorkspacePathCapacity + 1U, 'x'), 7U).valid());
    WorkspacePath noncanonical_path = path;
    noncanonical_path.storage.back() = 'x';
    CHECK_FALSE(noncanonical_path.valid());
}
TEST_CASE("file dialog client validates relative absolute and escaped selections", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-file-dialog-scope"};
    const auto root = temporary.path() / "root";
    std::filesystem::create_directory(root);
    const auto relative = make_dialog_helper(temporary.path(), "relative", "printf '%s\\n' child/item.txt");
    const auto absolute = make_dialog_helper(temporary.path(), "absolute", "printf '%s\\n' '" + (root / "absolute.txt").string() + "'");
    const auto escape = make_dialog_helper(temporary.path(), "escape", "printf '%s\\n' ../escape.txt");
    const FileDialogRequest request = dialog_request();
    const auto relative_result = run_dialog(relative, root, request);
    CHECK(relative_result.disposition == FileDialogDisposition::Selected);
    CHECK(relative_result.path.view() == (root / "child" / "item.txt").string());
    CHECK(run_dialog(absolute, root, request).disposition == FileDialogDisposition::Selected);
    CHECK(run_dialog(escape, root, request).disposition == FileDialogDisposition::Failed);
}
TEST_CASE("file dialog client drains complete immediate-exit selection output", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-file-dialog-immediate-output"};
    const auto helper = make_dialog_helper(temporary.path(), "immediate-output", "printf '%2048s%s\\n' '' selected.txt");
    const FileDialogResult result = run_dialog(helper, temporary.path(), dialog_request());
    CHECK(result.disposition == FileDialogDisposition::Selected);
    CHECK(result.path.view() == (temporary.path() / "selected.txt").string());
}
TEST_CASE("file dialog client preserves canonical mode and filter arguments", "[gui][services]") {
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-file-dialog-arguments"};
    const auto arguments = temporary.path() / "arguments";
    const auto helper = make_dialog_helper(temporary.path(), "arguments-helper", "printf '%s\\n' \"$@\" > '" + arguments.string() + "'; exit 1");
    const FileDialogRequest request = dialog_request("Save result", mmltk::controller::contracts::FileDialogMode::SaveFile, "Models", "*.onnx *.engine");
    CHECK(run_dialog(helper, temporary.path(), request).disposition == FileDialogDisposition::Cancelled);
    const std::string captured = read_file(arguments);
    CHECK(captured.find("--file-selection") != std::string::npos);
    CHECK(captured.find("--save") != std::string::npos);
    CHECK(captured.find("--confirm-overwrite") != std::string::npos);
    CHECK(captured.find("--file-filter=Models | *.onnx *.engine") != std::string::npos);
}
TEST_CASE("file dialog client bounds output and reaps cancellation-resistant helpers", "[gui][services]") {
    using namespace std::chrono_literals;
    mmltk::testsupport::ScopedTempDir temporary{"mmltk-file-dialog-reap"};
    const auto oversized = make_dialog_helper(temporary.path(), "oversized", "head -c 70000 /dev/zero");
    const FileDialogRequest request = dialog_request();
    const auto oversized_result = run_dialog(oversized, temporary.path(), request);
    CHECK(oversized_result.disposition == FileDialogDisposition::Failed);
    CHECK(oversized_result.error.view().find("capacity") != std::string_view::npos);
    const auto pid_fifo = temporary.path() / "helper.pid.fifo";
    REQUIRE(::mkfifo(pid_fifo.c_str(), 0600) == 0);
    ScopedFd readiness{::open(pid_fifo.c_str(), O_RDWR | O_CLOEXEC | O_NONBLOCK)};
    REQUIRE(readiness.get() >= 0);
    const auto resistant =
        make_dialog_helper(temporary.path(), "resistant", "printf '%s\\n' \"$$\" > '" + pid_fifo.string() + "'; trap '' TERM; while :; do sleep 1; done");
    FileDialogClientOwner owner{resistant.string(), temporary.path().string()};
    auto [cancellation, cancellation_token] = FileDialogCancellationSource::Mint();
    FileDialogResult result;
    std::thread runner([&] { result = owner.client().run(request, std::move(cancellation_token)); });
    pollfd ready{.fd = readiness.get(), .events = POLLIN, .revents = 0};
    const int readiness_result = ::poll(&ready, 1U, 5000);
    if (readiness_result != 1 || (ready.revents & POLLIN) == 0) {
        CHECK(cancellation.RequestCancel());
        runner.join();
        FAIL("file-dialog helper did not publish its readiness event");
    }
    std::array<char, 32U> pid_bytes{};
    const ssize_t pid_size = ::read(readiness.get(), pid_bytes.data(), pid_bytes.size());
    if (pid_size <= 0) {
        CHECK(cancellation.RequestCancel());
        runner.join();
        FAIL("file-dialog helper readiness event did not contain its pid");
    }
    const pid_t helper_pid = static_cast<pid_t>(std::stol(std::string{pid_bytes.data(), static_cast<std::size_t>(pid_size)}));
    CHECK(cancellation.RequestCancel());
    runner.join();
    CHECK(result.disposition == FileDialogDisposition::Reaped);
    errno = 0;
    CHECK(::waitpid(helper_pid, nullptr, WNOHANG) == -1);
    CHECK(errno == ECHILD);
    errno = 0;
    CHECK(::kill(helper_pid, 0) == -1);
    CHECK(errno == ESRCH);
}
}  // namespace mmltk::controller::services
