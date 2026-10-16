#include <catch2/catch_test_macros.hpp>
#include "src/frameworks/process/subprocess_utils.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <string>
#include <utility>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#include "encoder_training_fixture.h"
#include "detail/training_session_checkpoint.h"
#include "src/test_support/filesystem_test_utils.hpp"
namespace {
namespace process = mmltk::frameworks::process;
std::array<int, 2> selected_devices(int count) {
 std::array<int, 2> devices{0, 1};
 if (const auto* selected = std::getenv("MMLTK_TRAINING_TEST_DEVICE_IDS")) {
  const std::string text(selected);
  const auto comma = text.find(',');
  REQUIRE(comma != std::string::npos);
  devices = {std::stoi(text.substr(0, comma)), std::stoi(text.substr(comma + 1))};
 }
 REQUIRE(devices[0] >= 0);
 REQUIRE(devices[1] >= 0);
 REQUIRE(devices[0] < count);
 REQUIRE(devices[1] < count);
 REQUIRE(devices[0] != devices[1]);
 return devices;
}
auto start_distributed_workers(const std::filesystem::path& store, const std::array<int, 2>& devices, const std::string& scenario) {
 std::array<std::future<process::CapturedChildProcessResult>, 2> children;
 for (std::size_t rank = 0; rank < children.size(); ++rank)
  children[rank] = std::async(std::launch::async, [store, devices, scenario, rank] {
   process::ArgvBuffer arguments({MMLTK_DISTRIBUTED_TRAINING_WORKER, store.string(), std::to_string(rank), std::to_string(devices[rank]), scenario});
   return process::run_captured_child_process("training-nccl", "distributed training helper", [&](int output, int errors) {
    process::prepare_captured_output_child(output, errors);
    ::execv(arguments.program(), arguments.data());
    process::fail_child_setup(errors, process::ChildSetupStage::Exec);
   }, {}, std::chrono::seconds(600));
  });
 return children;
}
}  // namespace
TEST_CASE("Two selected GPUs retain the global training objective and early NCCL overlap", "[rfdetr][training][distributed][nccl]") {
 const auto count = mmltk::testsupport::checked_cuda_device_count();
 if (count < 2) SKIP("Two CUDA devices unavailable; NCCL training coverage unverified");
 const auto devices = selected_devices(count);
 const auto store = std::filesystem::temp_directory_path() / ("mmltk-training-nccl-" + std::to_string(::getpid()));
 std::filesystem::remove(store);
 namespace process = mmltk::frameworks::process;
 std::string scenario;
 SECTION("complete trajectory, target preparation and count handoff on empty ranks") { scenario = "trajectory"; }
 SECTION("PE grouped training and exact Resume") { scenario = "family-rf-detr-femto"; }
 SECTION("DINO Base grouped training and exact Resume") { scenario = "family-rf-detr-xlarge"; }
 SECTION("Keypoint grouped training and exact Resume on empty ranks") { scenario = "family-rf-detr-keypoint-preview"; }
 SECTION("first-rank cancellation aborts waiting peer") { scenario = "cancel"; }
 SECTION("failure after an early bucket retains custody and aborts waiting peer") { scenario = "early-failure"; }
 SECTION("cancellation retains a real in-flight count collective") { scenario = "cancel-count"; }
 SECTION("cancellation retains a real in-flight metric collective") { scenario = "cancel-metric"; }
 SECTION("cancellation retains real in-flight coalesced broadcasts") { scenario = "cancel-broadcast"; }
 CAPTURE(scenario);
 auto children = start_distributed_workers(store, devices, scenario);
 for (std::size_t rank = 0; rank < children.size(); ++rank) {
  CAPTURE(rank);
  process::CapturedChildProcessResult result;
  try {
   result = children[rank].get();
  } catch (const std::exception& error) {
   FAIL_CHECK(error.what());
   continue;
  }
  INFO(result.output);
  CHECK_FALSE(result.setup_failure.has_value());
  if (scenario == "trajectory" || scenario.starts_with("family-")) {
   CHECK(WIFEXITED(result.status));
   CHECK(WEXITSTATUS(result.status) == 0);
  } else {
   if (rank == 0) {
    const auto expected = scenario.starts_with("cancel-") ? "injected " + scenario.substr(7) + " cancellation"
                          : scenario == "early-failure"   ? std::string("injected failure after early gradient bucket")
                                                          : std::string("injected training cancellation");
    const auto first = result.output.find(expected);
    CHECK(first != std::string::npos);
    if (first != std::string::npos) CHECK(result.output.find(expected, first + expected.size()) == std::string::npos);
    if (scenario == "early-failure") CHECK(result.output.find("early bucket failure custody verified") != std::string::npos);
   }
   if (rank == 1 && scenario.starts_with("cancel-")) CHECK(result.output.find(scenario.substr(7) + " collective terminal custody verified") != std::string::npos);
   CHECK(((WIFEXITED(result.status) && WEXITSTATUS(result.status) != 0) || WIFSIGNALED(result.status)));
   CHECK(result.output.find("cancelled peer unexpectedly completed") == std::string::npos);
  }
 }
 std::filesystem::remove(store);
}
TEST_CASE("distributed scratch workers share admitted initialization and retire a rank zero import failure", "[rfdetr][training][distributed][encoder_pretraining][nccl]") {
 namespace r = mmltk::backend::models::rfdetr;
 const auto evidence = r::testsupport::encoder_training_evidence();
 if (!evidence) SKIP("real distributed scratch requires explicit MMLTK_RFDETR_ENCODER_PRETRAINING_DIR");
 const auto count = mmltk::testsupport::checked_cuda_device_count();
 if (count < 2) SKIP("two CUDA devices are required for distributed scratch");
 const auto devices = selected_devices(count);
 const mmltk::testsupport::ScopedTempDir root("distributed-scratch");
 auto request = r::testsupport::encoder_training_request(root.path(), *evidence);
 const auto store = root.path() / "rendezvous";
 const auto run = [&](bool success) {
  std::filesystem::remove(store);
  {
   std::ofstream file(store.string() + ".request.json");
   file << r::encode_train_request_json(request);
  }
  auto children = start_distributed_workers(store, devices, "scratch-run");
  std::array<std::string, 2> output;
  for (std::size_t rank = 0; rank < children.size(); ++rank) {
   CAPTURE(rank);
   process::CapturedChildProcessResult result;
   try {
    result = children[rank].get();
   } catch (const std::exception& error) {
    FAIL_CHECK(error.what());
    continue;
   }
   output[rank] = std::move(result.output);
   INFO(output[rank]);
   CHECK_FALSE(result.setup_failure);
   CHECK(WIFEXITED(result.status));
   if (WIFEXITED(result.status)) CHECK((WEXITSTATUS(result.status) == 0) == success);
  }
  return output;
 };
 run(true);
 const auto initial_path = request.output_dir / "session.json";
 const r::TrainingSessionAdmission initial(initial_path);
 REQUIRE(initial.model(0).metadata.encoder_pretraining);
 r::testsupport::require_reusable_encoder_request(initial.manifest().request);
 const auto alias = request.encoder_weights_path;
 std::filesystem::remove(alias);
 r::normalize_train_resume_request(request, initial_path);
 request.output_dir = root.path() / "resumed";
 request.epochs = 2;
 run(true);
 const r::TrainingSessionAdmission resumed(request.output_dir / "session.json");
 CHECK(resumed.manifest().initialization == initial.manifest().initialization);
 CHECK(resumed.model(0).metadata.encoder_pretraining == initial.model(0).metadata.encoder_pretraining);
 request.resume_path.clear();
 request.from_scratch = true;
 request.encoder_weights_path = alias;
 request.output_dir = root.path() / "rejected";
 std::ofstream(alias) << "invalid original source";
 const auto rejected = run(false);
 CHECK(rejected[0].find("encoder pretraining: source byte count differs from catalog") != std::string::npos);
 CHECK(rejected[1].find("training peer cancelled") != std::string::npos);
 CHECK_FALSE(std::filesystem::exists(request.output_dir / "session.json"));
}
