#include "cpu_affinity.h"
#include "dataset_compiler.h"
#include "dataset_loader.h"
#include "dataset/compile/dataset_compiler_internal.h"
#include "test_fixture.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace fastloader;
using namespace fastloader::testsupport;

namespace fs = std::filesystem;

namespace {

std::string make_unique_root_dir(const std::string& prefix) {
    std::string pattern = (fs::temp_directory_path() / (prefix + "_XXXXXX")).string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    char* created = ::mkdtemp(buffer.data());
    assert(created != nullptr);
    return std::string(created);
}

void expect_compile_failure(const FixtureSpec& fixture,
                            const std::function<void(const fs::path&)>& mutate,
                            const std::string& expected_error,
                            int target_width = -1,
                            int target_height = -1) {
    create_synthetic_dataset(fixture);
    const fs::path annotation_path = fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl";
    mutate(annotation_path);

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = fixture.root_dir + "/compiled";
    config.split = fixture.split;
    config.target_width = static_cast<uint32_t>(target_width > 0 ? target_width : fixture.width);
    config.target_height = static_cast<uint32_t>(target_height > 0 ? target_height : fixture.height);
    config.num_workers = 2;

    bool threw = false;
    try {
        DatasetCompiler::compile(config);
    } catch (const std::runtime_error& error) {
        threw = true;
        assert(std::string(error.what()).find(expected_error) != std::string::npos);
    }
    assert(threw);
}

void overwrite_annotation(const fs::path& annotation_path, const std::string& record) {
    std::ofstream file(annotation_path, std::ios::trunc);
    assert(file.is_open());
    file << record << "\n";
}

void test_vanished_masks_are_dropped_and_reported() {
    const FixtureSpec fixture{
        make_unique_root_dir("fastloader_compile_drop_vanished_mask"),
        "train",
        16,
        16,
        20,
    };
    create_synthetic_dataset(fixture);
    overwrite_annotation(
        fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl",
        R"({"class":"person","bbox_xyxy":[0,0,1,1],"mask_rle_encoding":"row_major_start_length","mask_rle":"0:1","image_size_wh":[16,16]})");

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = compiled_dir(fixture);
    config.split = fixture.split;
    config.target_width = 8;
    config.target_height = 8;
    config.num_workers = 2;

    std::vector<size_t> observed_drops;
    DatasetCompiler::compile(config, [&](const CompileProgress& progress) {
        observed_drops.push_back(progress.dropped_annotations);
    });

    assert(!observed_drops.empty());
    assert(observed_drops.back() == 1);
    assert(std::any_of(observed_drops.begin(),
                       observed_drops.end(),
                       [](size_t dropped) { return dropped == 1; }));

    DatasetLoader::Config loader_config;
    loader_config.compiled_path = compiled_bin_path(fixture);
    loader_config.batch_size = 1;
    loader_config.shuffle = false;
    DatasetLoader loader(loader_config);
    assert(loader.num_label_instances() == 9);
    assert(loader.label_index()[10].num_instances == 0);
}

void test_partial_mask_vanish_keeps_instance() {
    const FixtureSpec fixture{
        make_unique_root_dir("fastloader_compile_keep_partial_mask"),
        "train",
        16,
        16,
        20,
    };
    create_synthetic_dataset(fixture);
    overwrite_annotation(
        fs::path(dataset_dir(fixture)) / fixture.split / "000011.jsonl",
        R"({"class":"person","bbox_xyxy":[0,0,3,3],"mask_rle_encoding":"row_major_start_length","mask_rle":"0:2 16:2 34:1","image_size_wh":[16,16]})");

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = compiled_dir(fixture);
    config.split = fixture.split;
    config.target_width = 8;
    config.target_height = 8;
    config.num_workers = 2;

    std::vector<size_t> observed_drops;
    DatasetCompiler::compile(config, [&](const CompileProgress& progress) {
        observed_drops.push_back(progress.dropped_annotations);
    });

    assert(!observed_drops.empty());
    assert(observed_drops.back() == 0);

    DatasetLoader::Config loader_config;
    loader_config.compiled_path = compiled_bin_path(fixture);
    loader_config.batch_size = 1;
    loader_config.shuffle = false;
    DatasetLoader loader(loader_config);
    assert(loader.num_label_instances() == 10);
    const LabelIndexEntry& entry = loader.label_index()[10];
    assert(entry.num_instances == 1);
    const PackedInstance& instance = loader.label_data()[entry.label_begin];
    assert(instance.mask_rle_pairs > 0);
}

void test_compile_default_workers_use_full_cpuset() {
    const std::vector<int> allowed = allowed_cpu_set();
    assert(!allowed.empty());
    assert(compiler_internal::resolve_num_workers(0) == static_cast<int>(allowed.size()));
    assert(compiler_internal::resolve_num_workers(-1) == static_cast<int>(allowed.size()));
}

void test_invalid_annotations_fail_loud() {
    expect_compile_failure(
        FixtureSpec{
            make_unique_root_dir("fastloader_compile_invalid_json"),
            "train",
            65,
            65,
            20,
        },
        [](const fs::path& annotation_path) {
            std::ofstream file(annotation_path, std::ios::trunc);
            assert(file.is_open());
            file << "{invalid json}\n";
        },
        "invalid JSON annotation record");

    expect_compile_failure(
        FixtureSpec{
            make_unique_root_dir("fastloader_compile_unknown_class"),
            "train",
            65,
            65,
            20,
        },
        [](const fs::path& annotation_path) {
            std::ofstream file(annotation_path, std::ios::trunc);
            assert(file.is_open());
            file << R"({"class":"unknown","bbox_xyxy":[10,10,20,20],"mask_rle_encoding":"row_major_start_length","mask_rle":"660:10","image_size_wh":[65,65]})"
                 << "\n";
        },
        "is not declared in categories.json");

}

} // namespace

int main() {
    const FixtureSpec fixture{
        make_unique_root_dir("fastloader_compile_progress"),
        "train",
        257,
        193,
        96,
    };
    create_synthetic_dataset(fixture);

    CompilerConfig config;
    config.source_dir = dataset_dir(fixture);
    config.output_dir = fixture.root_dir + "/compiled";
    config.split = fixture.split;
    config.target_width = 97;
    config.target_height = 73;
    config.num_workers = 4;

    const std::thread::id main_thread_id = std::this_thread::get_id();
    std::vector<size_t> observed_done;
    std::vector<size_t> observed_totals;
    std::vector<CompileProgressPhase> observed_phases;
    std::vector<size_t> observed_active;
    std::vector<std::thread::id> callback_threads;
    DatasetCompiler::compile(config, [&](const CompileProgress& progress) {
        observed_done.push_back(progress.done);
        observed_totals.push_back(progress.total);
        observed_phases.push_back(progress.phase);
        observed_active.push_back(progress.active);
        callback_threads.push_back(std::this_thread::get_id());
    });

    const size_t expected_total = static_cast<size_t>(fixture.num_images) * 2 + 1;
    assert(!observed_done.empty());
    assert(observed_done.front() == 0);
    for (size_t index = 1; index < observed_done.size(); ++index) {
        assert(observed_done[index] >= observed_done[index - 1]);
    }
    assert(std::all_of(observed_totals.begin(),
                       observed_totals.end(),
                       [&](size_t total) { return total == expected_total; }));
    assert(observed_done.back() == expected_total);
    // With concurrent labels+pixels, we should see pixel phase reported
    // at some point when done >= num_images (pixels trailing)
    const bool saw_pixel_phase =
        std::any_of(observed_phases.begin(), observed_phases.end(),
                    [](CompileProgressPhase phase) { return phase == CompileProgressPhase::kPixels; });
    assert(saw_pixel_phase);
    const bool saw_active_pixel_worker =
        std::any_of(observed_active.begin(), observed_active.end(), [](size_t active) { return active > 0; });
    assert(saw_active_pixel_worker);

    assert(!callback_threads.empty());
    assert(std::all_of(callback_threads.begin(),
                       callback_threads.end(),
                       [&](const std::thread::id& id) { return id == callback_threads.front(); }));
    assert(callback_threads.front() != main_thread_id);

    test_compile_default_workers_use_full_cpuset();
    test_vanished_masks_are_dropped_and_reported();
    test_partial_mask_vanish_keeps_instance();
    test_invalid_annotations_fail_loud();

    return 0;
}
