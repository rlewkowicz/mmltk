#pragma once
#include <array>
#include <cstddef>
#include <string>
#include "src/backend/data/data_loading_options.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/frameworks/reflection/cli_declarations.h"
#include "src/frameworks/reflection/declaration_annotations.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
namespace mmltk::entrypoints::cli::root_options {
namespace data = mmltk::backend::data;
namespace reflection = mmltk::frameworks::reflection;
struct BenchCommandRequest final : data::DataLoadingOptions {
 MMLTK_MAX_PATH_BYTES std::string compiled_path;
 MMLTK_MINIMUM(std::size_t, 1U) std::size_t batch_size = 32U;
 MMLTK_MINIMUM(int, 1) int epochs = 1;
};
struct InfoCommandRequest final {
 MMLTK_MAX_PATH_BYTES std::string compiled_path;
};
MMLTK_REFLECT_FIELDS(BenchCommandRequest)
MMLTK_REFLECT_FIELDS(InfoCommandRequest)
using Compile = reflection::CliScope<data::CompilerConfig>;
using Bench = reflection::CliScope<BenchCommandRequest>;
using Info = reflection::CliScope<InfoCommandRequest>;
inline constexpr std::array kCompileOptions{
 MMLTK_CLI_OPTION(Compile, resize_mode, "Image geometry: Stretch or Letterbox", "Dataset"), MMLTK_CLI_OPTION(Compile, perceptual_downscale, "Perceptual shrinking", "Dataset"),
 MMLTK_CLI_OPTION(Compile, source_dir, "Source dataset directory", "Dataset", "source_dir", {}, true),
 MMLTK_CLI_OPTION(Compile, output_dir, "Compiled binary output directory", "Dataset", "output_dir", {}, true), MMLTK_CLI_OPTION(Compile, split, "Dataset split", "Dataset", "split", {}, true),
 MMLTK_CLI_NAMED(Compile, target_width, "--width", "Target image width", "Dataset"), MMLTK_CLI_NAMED(Compile, target_height, "--height", "Target image height", "Dataset"),
 MMLTK_CLI_OPTION(Compile, cuda_mask_batch_size, "CUDA mask batch size", "Execution"), MMLTK_CLI_OPTION(Compile, cuda_device_id, "CUDA device id", "Execution"),
 MMLTK_CLI_NAMED(Compile, num_workers, "--workers", "CPU worker budget", "Execution")
};
inline constexpr std::array kBenchOptions{
 reflection::negative_flag<BenchCommandRequest, &BenchCommandRequest::h2d_dataloader>("--gdrcopy", "Use GDRCopy image loading", "Execution"),
 MMLTK_CLI_OPTION(Bench, numa_node, "GPU-local NUMA node (-1 automatic)", "Execution"), MMLTK_CLI_NAMED(Bench, compiled_path, "--compiled", "Compiled dataset binary", "Dataset", "compiled", {}, true),
 MMLTK_CLI_OPTION(Bench, batch_size, "Streaming batch size", "Execution", "batch_size"), MMLTK_CLI_OPTION(Bench, epochs, "Number of epochs", "Execution", "num_epochs")
};
inline constexpr std::array kInfoOptions{MMLTK_CLI_NAMED(Info, compiled_path, "--compiled", "Compiled dataset binary", "Dataset", "compiled", {}, true)};
inline constexpr std::array kCompileUnexposed{reflection::unexposed<data::CompilerConfig, &data::CompilerConfig::worker_cpus>(
 "execution resolves the concrete CPU set from the container "
 "allocation")};
static_assert((reflection::audit_descriptors(kCompileOptions, kCompileUnexposed), true));
static_assert((reflection::audit_descriptors(kBenchOptions), true));
static_assert((reflection::audit_descriptors(kInfoOptions), true));
}  // namespace mmltk::entrypoints::cli::root_options
