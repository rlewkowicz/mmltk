#include "src/controller/services/run_output.h"
#include "src/controller/contracts/compute.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <fstream>
#include <future>
#include <set>
using mmltk::controller::services::reserve_run_output;
TEST_CASE("run output reserves independent workflow sequences and handles occupied names", "[controller][output]") {
 mmltk::testsupport::ScopedTempDir temp{"workflow-reservations"};
 for (const auto* name : {"train", "validate", "predict", "export"}) {
  const auto root = temp.path() / name;
  CHECK(reserve_run_output(root).filename() == "run-0001");
  std::ofstream(root / "run-0009");
  std::ofstream(root / "run-not-a-number");
  CHECK(reserve_run_output(root).filename() == "run-0010");
 }
 const auto root = temp.path() / "concurrent";
 std::array<std::future<std::filesystem::path>, 8> workers;
 for (auto& worker : workers) worker = std::async(std::launch::async, [&] { return reserve_run_output(root); });
 std::set<std::filesystem::path> paths;
 for (auto& worker : workers) {
  const auto path = worker.get();
  REQUIRE(std::filesystem::is_directory(path / ".mmltk-run-claim"));
  CHECK(paths.insert(path).second);
 }
 CHECK(reserve_run_output(root).filename() == "run-0009");
}
TEST_CASE("manual output uses empty roots while invalid and exhausted destinations fail", "[controller][output]") {
 mmltk::testsupport::ScopedTempDir temp{"workflow-manual-output"};
 const auto root = temp.path() / "manual";
 CHECK(reserve_run_output(root, true) == std::filesystem::absolute(root));
 CHECK(reserve_run_output(root, true).filename() == "run-0001");
 std::ofstream(root / "existing.txt") << "preserved";
 CHECK(reserve_run_output(root, true).filename() == "run-0002");
 CHECK(reserve_run_output(std::filesystem::relative(temp.path() / "relative"), true) == std::filesystem::absolute(temp.path() / "relative"));
 CHECK_THROWS(reserve_run_output({}));
 CHECK_THROWS(reserve_run_output("/proc/self/mmltk-output-unwritable"));
 CHECK_THROWS(reserve_run_output(root / "existing.txt"));
 CHECK_THROWS(reserve_run_output(root / "existing.txt" / "child"));
 for (const auto* suffix : {"run-18446744073709551615", "run-18446744073709551616"}) {
  const auto blocked = temp.path() / suffix;
  std::filesystem::create_directory(blocked);
  std::ofstream(blocked / suffix);
  CHECK_THROWS(reserve_run_output(blocked));
 }
}
TEST_CASE("output publication remains independent of failed and cancelled compute terminals", "[controller][output]") {
 namespace c = mmltk::controller::contracts;
 c::ComputeUiState state;
 for (const auto outcome : {c::ComputeOperationOutcome::Failed, c::ComputeOperationOutcome::Cancelled}) {
  c::begin_compute(state, state.generation_frontier + 1);
  CHECK(state.output.directory.empty());
  CHECK(state.output.artifacts.empty());
  state.output.directory = "/run";
  state.output.artifacts.emplace_back("/run/report.json");
  state.output.samples_directory = "/run/samples";
  state.output.completed_samples = 1000000;
  state.output.recent_sample = "/run/samples/latest.png";
  state.output.partial_video = "/run/prediction.partial.mkv";
  c::complete_compute(state, c::make_compute_terminal(outcome, state.generation_frontier));
  CHECK(state.terminal.valid_worker_terminal());
  CHECK(state.terminal.output.empty());
  CHECK(state.output.artifacts.size() == 1);
  CHECK(state.output.completed_samples == 1000000);
  CHECK(state.output.partial_video == "/run/prediction.partial.mkv");
 }
}
TEST_CASE("manual reservations exclusively claim absent and empty directories", "[controller][output]") {
 mmltk::testsupport::ScopedTempDir temp{"manual-output-collisions"};
 for (const bool existing : {false, true}) {
  const auto root = temp.path() / (existing ? "empty" : "absent");
  if (existing) std::filesystem::create_directory(root);
  std::array<std::future<std::filesystem::path>, 8> workers;
  for (auto& worker : workers) worker = std::async(std::launch::async, [&] { return reserve_run_output(root, true); });
  std::set<std::filesystem::path> paths;
  for (auto& worker : workers) {
   const auto path = worker.get();
   CHECK(paths.insert(path).second);
   CHECK(std::filesystem::is_directory(path / ".mmltk-run-claim"));
  }
  CHECK(paths.contains(std::filesystem::absolute(root)));
  CHECK(paths.insert(reserve_run_output(root, true)).second);
  const auto numbered = reserve_run_output(root);
  const auto selected_numbered = reserve_run_output(numbered, true);
  CHECK(selected_numbered != numbered);
  CHECK(selected_numbered.parent_path() == numbered);
 }
}
