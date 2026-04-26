#include "common_utils.h"
#include "cli_bootstrap.h"
#include "cli_bundled_tests.h"
#include "compiled_file_utils.h"
#include "compiled_format.h"
#include "dataset_compiler.h"
#include "dataset_loader.h"
#include "execution_policy.h"
#include "mmltk/model/cli_module.h"
#include "mmltk_logging.h"
#include "profile_utils.h"
#include "spdmon/spdmon.hpp"
#include "CLI11.hpp"
#if MMLTK_BUILD_RFDETR_NATIVE
#include "mmltk/rfdetr/module.h"
#endif
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

using namespace mmltk;

namespace {

constexpr int kCliParseSuccess = static_cast<int>(CLI::ExitCodes::Success);

int handle_parse_error(CLI::App& app, const CLI::ParseError& error) {
    app.exit(error);
    return error.get_exit_code() == kCliParseSuccess ? 0 : 1;
}

void report_cli_error(std::string_view message) noexcept {
    try {
        mmltk::logging::logger("cli")->error("{}", message);
        return;
    } catch (...) {
        std::fprintf(stderr, "%.*s\n", static_cast<int>(message.size()), message.data());
        return;
    }
}

std::vector<const mmltk::model::CliModule*> discover_builtin_cli_modules() {
    std::vector<const mmltk::model::CliModule*> modules;
#if MMLTK_BUILD_RFDETR_NATIVE
    modules.push_back(&mmltk::rfdetr::cli_module());
#endif
    return modules;
}

#if MMLTK_BUILD_RFDETR_NATIVE
const mmltk::model::CliModule* find_builtin_cli_module(const std::vector<const mmltk::model::CliModule*>& modules,
                                                       std::string_view command_name) {
    for (const auto* module : modules) {
        if (module != nullptr && module->command_name() == command_name) {
            return module;
        }
    }
    return nullptr;
}
#endif

CLI::Option* add_path_option(CLI::App* command, const std::string& name, std::string& value, const char* description,
                             bool required = false) {
    auto* option = command->add_option(name, value, description)->type_name("PATH");
    if (required) {
        option->required();
    }
    return option;
}

void cmd_compile(const CompilerConfig& cfg) {
    MMLTK_PROFILE_RUN_LABEL("mmltk compile");

    auto t0 = std::chrono::steady_clock::now();
    size_t last_done = 0;
    size_t total = 0;
    size_t progress_pulse = 0;
    spdmon::ProgressBar bar("compile", 0, "img");
    bar.set_postfix("scanning");
    DatasetCompiler::compile(cfg, [&](const CompileProgress& progress) {
        if (progress.total != total) {
            total = progress.total;
            bar.set_total(total);
            if (progress.done == 0 && total > 0) {
                bar.set_postfix("preparing");
            }
        }
        if (progress.done > last_done) {
            const size_t delta = progress.done - last_done;
            last_done = progress.done;
            bar.add(delta);
        }
        bar.set_postfix(spdmon::format_compile_postfix(progress, progress_pulse++));
    });
    bar.close();
    auto t1 = std::chrono::steady_clock::now();
    logging::logger("cli")->info("compile: done in {:.1f} seconds", std::chrono::duration<double>(t1 - t0).count());
}

void cmd_info(const std::string& compiled_path) {
    MMLTK_PROFILE_RUN_LABEL("mmltk info");
    const FileHeader header = read_compiled_header(compiled_path);
    printf("File: %s\n", compiled_path.c_str());
    printf("Images: %u\n", header.num_images);
    printf("Size: %ux%u\n", header.image_width, header.image_height);
    printf("Stride: %zu bytes/image (%.2f KB)\n", static_cast<size_t>(header.image_stride),
           static_cast<double>(header.image_stride) / 1024.0);
    printf("Classes: %u\n", header.num_classes);
    for (uint32_t i = 0; i < header.num_classes; ++i) {
        printf("  [%u] %s\n", i, header.class_names[i].data());
    }
    printf("Batches (bs=1): %u\n", header.num_images);
}

void cmd_bench(const DatasetLoader::Config& cfg, int num_epochs) {
    MMLTK_PROFILE_RUN_LABEL("mmltk bench");

    DatasetLoader loader(cfg);
    printf("Benchmarking: %zu images, batch=%zu, stride=%zu, epochs=%d\n", loader.num_images(), cfg.batch_size,
           loader.image_stride(), num_epochs);

    for (int e = 0; e < num_epochs; ++e) {
        auto t0 = std::chrono::steady_clock::now();
        loader.begin_epoch();
        Batch batch;
        size_t total = 0;
        while (loader.next_batch(batch)) {
            total += batch.num_images;
            loader.release_batch(batch);
        }
        loader.cuda().sync();
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        printf("Epoch %d: %zu images in %.2f sec (%.0f img/sec)\n", e, total, secs, static_cast<double>(total) / secs);
    }
}

}  // namespace

int main(int argc, char** argv) {
    try {
        logging::initialize(logging::merge(logging::config_from_env("mmltk"), logging::scan_cli_overrides(argc, argv)));
        const ExecutionPolicySnapshot execution_snapshot = apply_process_execution_policy();
        log_process_execution_policy("mmltk", execution_snapshot, false, true);
        const cli_support::ParsedTestRequest test_request = cli_support::parse_test_request(argc, argv);
        const auto bundled_test_bundles = cli_support::discover_bundled_test_bundles();
        const auto builtin_cli_modules = discover_builtin_cli_modules();
        if (test_request.requested) {
            return cli_support::handle_bundled_test_request(test_request, bundled_test_bundles);
        }
#if MMLTK_BUILD_RFDETR_NATIVE
        if (argc > 1) {
            if (const auto* cli_module = find_builtin_cli_module(builtin_cli_modules, argv[1])) {
                return cli_module->handle_cli(argc, argv);
            }
        }
#endif

        CLI::App app{"High-throughput dataset compilation, inspection, and streaming benchmarks"};
        app.name("mmltk");
        app.option_defaults()->always_capture_default();
        app.require_subcommand(1);
        app.footer([bundled_test_bundles]() { return cli_support::bundled_test_help_text(bundled_test_bundles); });
        cli_support::LoggingOptions logging_options;
        cli_support::add_logging_options(app, logging_options);
        std::string test_bundle_option;
        app.add_option("--test", test_bundle_option,
                       "Run bundled Catch2 suites by bundle name (`list`, `core`, `gui`, `rfdetr`, or `all`)")
            ->type_name("BUNDLE");

        CompilerConfig compile_config;
        auto* compile_cmd =
            app.add_subcommand("compile", "Compile a raw dataset split into mmltk's mmap-friendly binary format");
        add_path_option(compile_cmd, "--source-dir,source_dir", compile_config.source_dir, "Source dataset directory",
                        true);
        add_path_option(compile_cmd, "--output-dir,output_dir", compile_config.output_dir,
                        "Output directory for compiled binaries", true);
        compile_cmd->add_option("--split,split", compile_config.split, "Dataset split name")->required();
        auto* compile_width =
            compile_cmd->add_option("--width,width", compile_config.target_width, "Target image width in pixels");
        auto* compile_height =
            compile_cmd->add_option("--height,height", compile_config.target_height, "Target image height in pixels");
        auto* compile_cuda_mask_batch =
            compile_cmd->add_option("--cuda-mask-batch-size", compile_config.cuda_mask_batch_size,
                                    "Batched CUDA mask-resize task count; 0 disables GPU mask resizing");
        auto* compile_cuda_device = compile_cmd->add_option("--cuda-device-id", compile_config.cuda_device_id,
                                                            "CUDA device id used for batched mask resizing");
        auto* compile_workers =
            compile_cmd->add_option("--workers", compile_config.num_workers,
                                    "Total CPU worker budget for compile; 0 or negative selects all available CPUs");
        compile_width->type_name("INT");
        compile_height->type_name("INT");
        compile_cuda_mask_batch->type_name("INT");
        compile_cuda_device->type_name("INT");
        compile_workers->type_name("INT");

        DatasetLoader::Config bench_config;
        bench_config.shuffle = true;
        bench_config.batch_size = 32;
        int bench_epochs = 1;
        auto* bench_cmd = app.add_subcommand("bench", "Benchmark streaming throughput over a compiled dataset");
        add_path_option(bench_cmd, "--compiled,compiled", bench_config.compiled_path, "Compiled dataset binary", true);
        bench_cmd
            ->add_option("--batch-size,batch_size", bench_config.batch_size,
                         "Batch size to stream during the benchmark")
            ->type_name("INT");
        bench_cmd->add_option("--epochs,num_epochs", bench_epochs, "Number of epochs to run during the benchmark")
            ->type_name("INT");

        std::string info_compiled_path;
        auto* info_cmd = app.add_subcommand("info", "Inspect metadata from a compiled dataset binary");
        add_path_option(info_cmd, "--compiled,compiled", info_compiled_path, "Compiled dataset binary", true);

        for (const auto* cli_module : builtin_cli_modules) {
            if (cli_module == nullptr) {
                continue;
            }
            app.add_subcommand(std::string(cli_module->command_name()), std::string(cli_module->summary()))
                ->footer("Run `mmltk " + std::string(cli_module->command_name()) +
                         " --help` for the full command surface.");
        }

        try {
            app.parse(argc, argv);
        } catch (const CLI::ParseError& error) {
            return handle_parse_error(app, error);
        }

        if (compile_cmd->parsed()) {
            if (compile_width->count() > 0 && compile_height->count() == 0) {
                compile_config.target_height = compile_config.target_width;
            }
            cmd_compile(compile_config);
            return 0;
        }
        if (bench_cmd->parsed()) {
            cmd_bench(bench_config, bench_epochs);
            return 0;
        }
        if (info_cmd->parsed()) {
            cmd_info(info_compiled_path);
            return 0;
        }
        return 1;
    } catch (const std::exception& error) {
        report_cli_error(std::string("mmltk error: ") + error.what());
    } catch (...) {
        report_cli_error("mmltk error: unknown exception");
    }
    return 1;
}
