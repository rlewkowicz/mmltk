#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <nlohmann/json.hpp>
#include "src/controller/browser/application_materializer.h"
#include "src/controller/services/settings_store.h"
#include "src/controller/shell/application_system_storage.h"
#include "src/test_support/async_test_utils.hpp"
#include "src/test_support/cuda_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/test_support/environment_test_utils.hpp"
namespace mmltk::controller::shell {
namespace {
TEST_CASE("production dataset factory connects optional tracing through staged overlap rejection", "[controller][shell][dataset][diagnostics][cuda]") {
 if (mmltk::testsupport::checked_cuda_device_count() == 0) SKIP("CUDA device unavailable for native application composition");
 const mmltk::testsupport::ScopedTempDir root{"shell-dataset-wiring"};
 const auto output = root.path() / "compiled";
 const auto cache = output / "cache";
 std::filesystem::create_directories(cache);
 const auto retained = cache / "retained.fixture";
 mmltk::testsupport::write_text_file(retained, "retained cache bytes");
 const auto retained_time = std::filesystem::last_write_time(retained);
 const mmltk::testsupport::ScopedEnvironmentVariable environment{"MMLTK_BENCHMARK_DATASET_CACHE_ROOT"};
 REQUIRE(::setenv("MMLTK_BENCHMARK_DATASET_CACHE_ROOT", cache.c_str(), 1) == 0);
 auto settings = contracts::default_gui_settings_state();
 settings.workflows.train.compile_benchmark_dataset_override = true;
 settings.workflows.train.overwrite_compiled_dataset = true;
 settings.workflows.train.compiled_dataset_dir = output;
 const services::SettingsLocation location{(root.path() / "settings.json").string()};
 REQUIRE(services::SettingsStore::save(location.value(), settings, 1U).succeeded());
 const auto log = root.path() / "trace.jsonl";
 services::DiagnosticsClient client{mmltk::common::io::ScopedFd{::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)},
                                    services::DiagnosticsExecutionPolicy::CallerDriven};
 REQUIRE(client.enabled());
 bool enabled = true;
 SECTION("enabled factory retains the target beyond configuration and creator lifetimes") {}
 SECTION("disabled factory supplies no benchmark observer") { enabled = false; }
 ApplicationSystemConfiguration configuration{
  .base_visual = {.device = 0, .maximum_width = 64U, .maximum_height = 64U},
  .output_visual = {.device = 0, .maximum_width = 256U, .maximum_height = 256U},
  .explore_nproc = 1U,
  .presentation = {.import_socket = root.path() / "workspace.sock"},
  .settings_location = location,
 };
 if (enabled) {
  services::RuntimeDiagnostics creator{client.producer()};
  configuration.dataset_diagnostics = creator.target();
 }
 const auto changed = browser::encode_system_event<&ApplicationSystems::dataset>(DatasetChanged{});
 std::promise<void> settled;
 ApplicationSystemStorage storage{std::move(configuration), [&](const browser::SystemEvent& event) {
                                   if (event.system_id == changed.system_id && event.event_id == changed.event_id) settled.set_value();
                                  }};
 const mmltk::testsupport::ScopedTestCleanup shutdown{[&] {
  storage.presentation().CloseAdmission();
  storage.presentation().BrowserPeerLost();
  static_cast<void>(storage.dataset().Stop());
  static_cast<void>(storage.presentation().Shutdown());
  storage.live().Shutdown();
  storage.upscale().Shutdown();
  storage.annotation().Shutdown();
  storage.explore().Shutdown();
  storage.predict().Shutdown();
  storage.export_system().Shutdown();
  storage.validation().Shutdown();
  storage.training().Shutdown();
  storage.model().Shutdown();
  storage.dataset().Shutdown();
  storage.file_dialog().Shutdown();
 }};
 static_cast<void>(storage.dataset().Compile({}));
 mmltk::testsupport::await_test_promise(settled, "production dataset overlap rejection", std::chrono::seconds{10});
 storage.dataset().Shutdown();
 const auto snapshot = storage.dataset().snapshot();
 CHECK_FALSE(snapshot.active);
 CHECK(snapshot.terminal.outcome == contracts::ArtifactTerminalOutcome::Failed);
 CHECK(snapshot.terminal.detail == "benchmark output and cache directories must not overlap");
 CHECK(std::filesystem::last_write_time(retained) == retained_time);
 CHECK(std::filesystem::file_size(retained) == std::string_view{"retained cache bytes"}.size());
 CHECK_FALSE(std::filesystem::exists(cache / "downloads"));
 client.close(services::DiagnosticsCloseMode::Flush);
 std::ifstream records{log};
 std::size_t path_records = 0U;
 std::size_t benchmark_records = 0U;
 for (std::string line; std::getline(records, line);) {
  const auto record = nlohmann::json::parse(line);
  if (record.value("kind", "") != "benchmark_dataset") continue;
  ++benchmark_records;
  if (record.value("name", "") != "benchmark.compile.paths") continue;
  ++path_records;
  CHECK(record.at("steady_ns").is_number_integer());
  CHECK(record.at("fields").at("cache_root") == cache.string());
  const std::filesystem::path staging{record.at("fields").at("output_root").get<std::string>()};
  CHECK(staging != output);
  CHECK(staging.parent_path() == output.parent_path());
  CHECK_FALSE(std::filesystem::exists(staging));
 }
 CHECK(path_records == (enabled ? 1U : 0U));
 if (!enabled) CHECK(benchmark_records == 0U);
}
}  // namespace
}  // namespace mmltk::controller::shell
