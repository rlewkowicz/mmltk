#include "gui/train_process_runtime.h"

#include "mmltk/rfdetr/workflow_requests.h"

#include "support/catch2_compat.hpp"
#include "support/filesystem_test_utils.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

fs::path make_temp_root(const char* pattern_name) {
    std::string temp_pattern = (fs::temp_directory_path() / (std::string(pattern_name) + ".XXXXXX")).string();
    std::vector<char> temp_buffer(temp_pattern.begin(), temp_pattern.end());
    temp_buffer.push_back('\0');
    const char* temp_root_raw = ::mkdtemp(temp_buffer.data());
    if (temp_root_raw == nullptr) {
        throw std::runtime_error("failed to create temp directory");
    }
    return temp_root_raw;
}

void write_text_file(const fs::path& path, std::string_view contents) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream stream(path, std::ios::trunc);
    if (!stream.is_open()) {
        throw std::runtime_error("failed to write file: " + path.string());
    }
    stream << contents;
}

void write_executable_script(const fs::path& path, std::string_view contents) {
    write_text_file(path, contents);
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);
}

mmltk::rfdetr::TrainRequest make_train_request(const fs::path& root) {
    mmltk::rfdetr::TrainRequest request;
    request.train_compiled_path = root / "train.bin";
    request.val_compiled_path = root / "val.bin";
    request.output_dir = root / "output";
    request.weights_path = root / "weights.pt";
    request.device_ids = {0};
    request.device_id = 0;
    request.batch_size = 1U;
    request.epochs = 1;
    return request;
}

mmltk::gui::LocalTrainSessionState wait_for_session_exit(mmltk::gui::LocalTrainSession& session) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = session.snapshot();
        if (!snapshot.running) {
            return snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    throw std::runtime_error("timed out waiting for local train session to finish");
}

void test_local_train_session_cleans_stale_artifacts_before_exec_failure() {
    const fs::path temp_root = make_temp_root("mmltk-local-train-cleanup");
    const fs::path output_dir = temp_root / "output";
    fs::create_directories(output_dir);
    write_text_file(output_dir / "progress.json", R"({"phase":"complete","epoch":99})");
    write_text_file(output_dir / "results.json", R"({"best_checkpoint":"stale.pt"})");
    write_text_file(output_dir / "log.txt", R"({"phase":"complete","epoch":99})");

    mmltk::gui::LocalTrainSession session;
    const mmltk::rfdetr::TrainRequest request = make_train_request(temp_root);
    session.start(request, temp_root / "missing-cli", "rf-detr-seg-medium");

    const mmltk::gui::LocalTrainSessionState state = wait_for_session_exit(session);
    session.shutdown();

    assert(!state.running);
    assert(state.exit_code == 127);
    assert(!state.progress.has_value());
    assert(state.last_error.find("local train child execv failed") != std::string::npos);
    assert(!fs::exists(output_dir / "progress.json"));
    assert(!fs::exists(output_dir / "results.json"));
    assert(!fs::exists(output_dir / "log.txt"));

    mmltk::testsupport::remove_path_recursively_best_effort(temp_root);
}

void test_local_train_session_appends_captured_output_to_exit_errors() {
    const fs::path temp_root = make_temp_root("mmltk-local-train-error");
    const fs::path script_path = temp_root / "fake-train.sh";
    write_executable_script(script_path,
                            "#!/usr/bin/env bash\n"
                            "printf 'fatal output line\\n'\n"
                            "exit 3\n");

    mmltk::gui::LocalTrainSession session;
    const mmltk::rfdetr::TrainRequest request = make_train_request(temp_root);
    session.start(request, script_path, "rf-detr-seg-medium");

    const mmltk::gui::LocalTrainSessionState state = wait_for_session_exit(session);
    session.shutdown();

    assert(!state.running);
    assert(state.exit_code == 3);
    assert(state.output_tail.find("fatal output line") != std::string::npos);
    assert(state.last_error.find("local train exited with code 3") != std::string::npos);
    assert(state.last_error.find("fatal output line") != std::string::npos);

    mmltk::testsupport::remove_path_recursively_best_effort(temp_root);
}

}  // namespace

MMLTK_REGISTER_TEST_CASE("[gui][local_train_runtime]",
                         test_local_train_session_cleans_stale_artifacts_before_exec_failure);
MMLTK_REGISTER_TEST_CASE("[gui][local_train_runtime]", test_local_train_session_appends_captured_output_to_exit_errors);
