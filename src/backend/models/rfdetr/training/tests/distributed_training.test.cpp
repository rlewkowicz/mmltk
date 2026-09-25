#include <catch2/catch_test_macros.hpp>
#include "src/frameworks/process/subprocess_utils.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
TEST_CASE("Two selected GPUs retain the global training objective and early NCCL overlap", "[rfdetr][training][distributed][nccl]") {
 const auto count = mmltk::testsupport::checked_cuda_device_count();
 if (count < 2) SKIP("Two CUDA devices unavailable; NCCL training coverage unverified");
 std::array<int, 2> devices{0, 1};
 if (const auto* selected = std::getenv("MMLTK_TRAINING_TEST_DEVICE_IDS")) {
  const std::string text(selected); const auto comma = text.find(',');
  REQUIRE(comma != std::string::npos);
  devices = {std::stoi(text.substr(0, comma)), std::stoi(text.substr(comma + 1))};
 }
 REQUIRE(devices[0] >= 0); REQUIRE(devices[1] >= 0); REQUIRE(devices[0] < count); REQUIRE(devices[1] < count); REQUIRE(devices[0] != devices[1]);
 const auto store = std::filesystem::temp_directory_path() / ("mmltk-training-nccl-" + std::to_string(::getpid()));
 std::filesystem::remove(store);
 namespace process = mmltk::frameworks::process;
 std::string scenario;
 SECTION("complete trajectory and overlap") { scenario = "trajectory"; }
 SECTION("first-rank cancellation aborts waiting peer") { scenario = "cancel"; }
 SECTION("failure after an early bucket retains custody and aborts waiting peer") { scenario = "early-failure"; }
 std::array<std::future<process::CapturedChildProcessResult>, 2> children;
 for (std::size_t rank = 0; rank < children.size(); ++rank) children[rank] = std::async(std::launch::async, [&, rank] {
  process::ArgvBuffer arguments({MMLTK_DISTRIBUTED_TRAINING_WORKER, store.string(), std::to_string(rank), std::to_string(devices[rank]), scenario});
  return process::run_captured_child_process("training-nccl", "distributed training helper", [&](int output, int errors) {
   process::prepare_captured_output_child(output, errors);
   ::execv(arguments.program(), arguments.data()); process::fail_child_setup(errors, process::ChildSetupStage::Exec);
  }, {}, std::chrono::seconds(180));
 });
 for (std::size_t rank = 0; rank < children.size(); ++rank) {
  const auto result = children[rank].get(); INFO(result.output);
  CHECK_FALSE(result.setup_failure.has_value());
  if (scenario == "trajectory") { CHECK(WIFEXITED(result.status)); CHECK(WEXITSTATUS(result.status) == 0); }
  else { if (rank == 0) {
   CHECK(result.output.find(scenario == "early-failure" ? "injected failure after early gradient bucket" : "injected training cancellation") != std::string::npos);
   if (scenario == "early-failure") CHECK(result.output.find("early bucket failure custody verified") != std::string::npos);
  } CHECK((WIFEXITED(result.status) && WEXITSTATUS(result.status) != 0) || WIFSIGNALED(result.status)); CHECK(result.output.find("cancelled peer unexpectedly completed") == std::string::npos); }
 }
 std::filesystem::remove(store);
}
