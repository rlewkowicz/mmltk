#include "common_utils.h"
#include "compiled_file_utils.h"
#include "compiled_format.h"
#include "dataset_compiler.h"
#include "dataset_loader.h"
#include "execution_policy.h"
#include "mmltk/model/cli_module.h"
#include "mmltk_logging.h"
#include "profile_utils.h"
#include "spdmon/spdmon.hpp"
#include "runtime_paths.h"
#include "CLI11.hpp"
#if MMLTK_BUILD_RFDETR_NATIVE
#include "mmltk/rfdetr/module.h"
#endif
#include <chrono>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

using namespace mmltk;

namespace {

namespace fs = std::filesystem;

constexpr int kCliParseSuccess = static_cast<int>(CLI::ExitCodes::Success);

struct TestBundleSpec {
    const char* name;
    const char* executable_name;
    const char* description;
};

struct BundledTestBundle {
    const TestBundleSpec* spec = nullptr;
    fs::path executable_path;
};

struct ParsedTestRequest {
    bool requested = false;
    std::string bundle_name;
    std::vector<std::string> forwarded_args;
};

constexpr std::array<TestBundleSpec, 3> kBundledTestBundleSpecs{{
    {"core", "mmltk_tests_core", "Core dataset/runtime/compiler Catch2 suite"},
    {"gui", "mmltk_tests_gui", "GUI state/options/runtime Catch2 suite"},
    {"rfdetr", "mmltk_tests_model_rfdetr", "RF-DETR native/model Catch2 suite"},
}};

int handle_parse_error(CLI::App& app, const CLI::ParseError& error) {
    app.exit(error);
    return error.get_exit_code() == kCliParseSuccess ? 0 : 1;
}

bool is_executable_file(const fs::path& path) {
    std::error_code status_error;
    const fs::file_status status = fs::status(path, status_error);
    if (status_error || !fs::is_regular_file(status)) {
        return false;
    }
    return ::access(path.c_str(), X_OK) == 0;
}

std::vector<BundledTestBundle> discover_bundled_test_bundles() {
    const fs::path executable_dir = runtime_paths::current_executable_path().parent_path();
    std::vector<BundledTestBundle> bundles;
    bundles.reserve(std::size(kBundledTestBundleSpecs));
    for (const auto& spec : kBundledTestBundleSpecs) {
        const fs::path executable_path = executable_dir / spec.executable_name;
        if (is_executable_file(executable_path)) {
            bundles.push_back(BundledTestBundle{
                &spec,
                executable_path,
            });
        }
    }
    return bundles;
}

std::string bundled_test_help_text(const std::vector<BundledTestBundle>& bundles) {
    std::string help_text =
        "tests:\n"
        "  run `mmltk --test <bundle> [-- <Catch2 args...>]` to execute bundled Catch2 suites\n"
        "  use `mmltk --test list` to print the bundles available beside this executable\n"
        "  example: `mmltk --test core -- --list-tests`\n";
    if (bundles.empty()) {
        help_text += "  bundled test bundles beside this executable: none\n";
        return help_text;
    }
    help_text += "  bundled test bundles beside this executable:\n";
    for (const auto& bundle : bundles) {
        help_text += "    ";
        help_text += bundle.spec->name;
        help_text += "  ";
        help_text += bundle.spec->description;
        help_text += "\n";
    }
    help_text += "    all  Run every available bundled Catch2 suite\n";
    return help_text;
}

void print_bundled_test_bundles(FILE* stream, const std::vector<BundledTestBundle>& bundles) {
    std::fprintf(stream, "Bundled test bundles\n");
    if (bundles.empty()) {
        std::fprintf(stream, "  none found beside this mmltk executable\n");
    } else {
        for (const auto& bundle : bundles) {
            std::fprintf(stream, "  %-6s %s\n", bundle.spec->name, bundle.spec->description);
        }
        std::fprintf(stream, "  %-6s %s\n", "all", "Run every available bundled Catch2 suite");
    }
    std::fprintf(stream,
                 "\nUse: mmltk --test <bundle> [-- <Catch2 args...>]\n"
                 "Example: mmltk --test core -- --list-tests\n");
}

const BundledTestBundle* find_bundled_test_bundle(const std::vector<BundledTestBundle>& bundles,
                                                  std::string_view bundle_name) {
    for (const auto& bundle : bundles) {
        if (std::string_view(bundle.spec->name) == bundle_name) {
            return &bundle;
        }
    }
    return nullptr;
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

int run_bundled_test_bundle(const BundledTestBundle& bundle,
                            const std::vector<std::string>& forwarded_args,
                            bool announce_bundle) {
    if (announce_bundle) {
        logging::logger("cli")->info("mmltk --test: running bundle `{}` ({})",
                                     bundle.spec->name,
                                     bundle.executable_path.string());
    }

    std::vector<std::string> argv_strings;
    argv_strings.reserve(1 + forwarded_args.size());
    argv_strings.push_back(bundle.executable_path.string());
    argv_strings.insert(argv_strings.end(), forwarded_args.begin(), forwarded_args.end());

    std::vector<char*> raw_argv;
    raw_argv.reserve(argv_strings.size() + 1);
    for (std::string& arg : argv_strings) {
        raw_argv.push_back(arg.data());
    }
    raw_argv.push_back(nullptr);

    const pid_t child_pid = ::fork();
    if (child_pid < 0) {
        throw std::runtime_error(std::string("failed to fork bundled test runner: ") +
                                 std::strerror(errno));
    }
    if (child_pid == 0) {
        ::execv(raw_argv.front(), raw_argv.data());
        logging::logger("cli")->error("execv({}) failed: {}", raw_argv.front(), std::strerror(errno));
        std::_Exit(127);
    }

    int status = 0;
    while (::waitpid(child_pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error(std::string("failed waiting for bundled test runner: ") +
                                 std::strerror(errno));
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return 1;
}

int handle_bundled_test_request(const ParsedTestRequest& request,
                                const std::vector<BundledTestBundle>& bundles) {
    if (request.bundle_name.empty()) {
        logging::logger("cli")->error("mmltk --test requires a bundle name or `list`");
        print_bundled_test_bundles(stderr, bundles);
        return 1;
    }
    if (request.bundle_name == "list" || request.bundle_name == "help" ||
        request.bundle_name == "--help" || request.bundle_name == "-h") {
        print_bundled_test_bundles(stdout, bundles);
        return 0;
    }
    if (request.bundle_name == "all") {
        if (bundles.empty()) {
            logging::logger("cli")->error(
                "mmltk --test: no bundled test suites were found beside this executable");
            return 1;
        }
        int first_failure = 0;
        for (size_t index = 0; index < bundles.size(); ++index) {
            const int exit_code = run_bundled_test_bundle(
                bundles[index], request.forwarded_args, bundles.size() > 1U || index > 0U);
            if (exit_code != 0 && first_failure == 0) {
                first_failure = exit_code;
            }
        }
        return first_failure;
    }

    const BundledTestBundle* bundle = find_bundled_test_bundle(bundles, request.bundle_name);
    if (bundle == nullptr) {
        logging::logger("cli")->error("mmltk --test: unknown test bundle `{}`", request.bundle_name);
        print_bundled_test_bundles(stderr, bundles);
        return 1;
    }
    return run_bundled_test_bundle(*bundle, request.forwarded_args, false);
}

std::vector<const mmltk::model::CliModule*> discover_builtin_cli_modules() {
    std::vector<const mmltk::model::CliModule*> modules;
#if MMLTK_BUILD_RFDETR_NATIVE
    modules.push_back(&mmltk::rfdetr::cli_module());
#endif
    return modules;
}

#if MMLTK_BUILD_RFDETR_NATIVE
const mmltk::model::CliModule* find_builtin_cli_module(
    const std::vector<const mmltk::model::CliModule*>& modules,
    std::string_view command_name) {
    for (const auto* module : modules) {
        if (module != nullptr && module->command_name() == command_name) {
            return module;
        }
    }
    return nullptr;
}
#endif

ParsedTestRequest parse_test_request(int argc, char** argv) {
    ParsedTestRequest request;
    if (argc < 2) {
        return request;
    }

    const std::string_view first_arg = argv[1];
    constexpr std::string_view test_prefix = "--test=";
    int next_index = 2;
    if (first_arg == "--test") {
        request.requested = true;
        if (argc >= 3) {
            request.bundle_name = argv[2];
            next_index = 3;
        }
    } else if (first_arg.size() >= test_prefix.size() &&
               first_arg.substr(0, test_prefix.size()) == test_prefix) {
        request.requested = true;
        request.bundle_name = std::string(first_arg.substr(test_prefix.size()));
        next_index = 2;
    } else {
        return request;
    }

    request.forwarded_args.reserve(static_cast<size_t>(argc - next_index));
    for (int index = next_index; index < argc; ++index) {
        request.forwarded_args.emplace_back(argv[index]);
    }
    if (!request.forwarded_args.empty() && request.forwarded_args.front() == "--") {
        request.forwarded_args.erase(request.forwarded_args.begin());
    }
    return request;
}

CLI::Option* add_path_option(CLI::App* command,
                             const std::string& name,
                             std::string& value,
                             const char* description,
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
    logging::logger("cli")->info("compile: done in {:.1f} seconds",
                                 std::chrono::duration<double>(t1 - t0).count());
}

void cmd_info(const std::string& compiled_path) {
    MMLTK_PROFILE_RUN_LABEL("mmltk info");
    const FileHeader header = read_compiled_header(compiled_path);
    printf("File: %s\n", compiled_path.c_str());
    printf("Images: %u\n", header.num_images);
    printf("Size: %ux%u\n", header.image_width, header.image_height);
    printf("Stride: %zu bytes/image (%.2f KB)\n",
           static_cast<size_t>(header.image_stride),
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
    printf("Benchmarking: %zu images, batch=%zu, stride=%zu, epochs=%d\n",
           loader.num_images(), cfg.batch_size, loader.image_stride(), num_epochs);

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
        printf("Epoch %d: %zu images in %.2f sec (%.0f img/sec)\n",
               e, total, secs, static_cast<double>(total) / secs);
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        logging::initialize(logging::merge(logging::config_from_env("mmltk"),
                                           logging::scan_cli_overrides(argc, argv)));
        const ExecutionPolicySnapshot execution_snapshot = apply_process_execution_policy();
        log_process_execution_policy("mmltk", execution_snapshot, false, true);
        const ParsedTestRequest test_request = parse_test_request(argc, argv);
        const auto builtin_cli_modules = discover_builtin_cli_modules();
        if (test_request.requested) {
            return handle_bundled_test_request(test_request, discover_bundled_test_bundles());
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
        app.footer([]() { return bundled_test_help_text(discover_bundled_test_bundles()); });
        std::string log_level_option;
        std::string log_file_option;
        std::string log_dir_option;
        app.add_option("--log-level", log_level_option,
                       "Logging level (trace, debug, info, warn, error, critical, off)");
        app.add_option("--log-file", log_file_option, "Explicit log file path")
            ->type_name("PATH");
        app.add_option("--log-dir", log_dir_option,
                       "Log directory for the default rotating file sink")
            ->type_name("PATH");
        std::string test_bundle_option;
        app.add_option("--test", test_bundle_option,
                       "Run bundled Catch2 suites by bundle name (`list`, `core`, `gui`, `rfdetr`, or `all`)")
            ->type_name("BUNDLE");

        CompilerConfig compile_config;
        auto* compile_cmd = app.add_subcommand(
            "compile",
            "Compile a raw dataset split into mmltk's mmap-friendly binary format");
        add_path_option(compile_cmd, "--source-dir,source_dir", compile_config.source_dir,
                        "Source dataset directory", true);
        add_path_option(compile_cmd, "--output-dir,output_dir", compile_config.output_dir,
                        "Output directory for compiled binaries", true);
        compile_cmd->add_option("--split,split", compile_config.split, "Dataset split name")
            ->required();
        auto* compile_width = compile_cmd->add_option(
            "--width,width",
            compile_config.target_width,
            "Target image width in pixels");
        auto* compile_height = compile_cmd->add_option(
            "--height,height",
            compile_config.target_height,
            "Target image height in pixels");
        auto* compile_cuda_mask_batch = compile_cmd->add_option(
            "--cuda-mask-batch-size",
            compile_config.cuda_mask_batch_size,
            "Batched CUDA mask-resize task count; 0 disables GPU mask resizing");
        auto* compile_cuda_device = compile_cmd->add_option(
            "--cuda-device-id",
            compile_config.cuda_device_id,
            "CUDA device id used for batched mask resizing");
        auto* compile_workers = compile_cmd->add_option(
            "--workers",
            compile_config.num_workers,
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
        auto* bench_cmd = app.add_subcommand(
            "bench",
            "Benchmark streaming throughput over a compiled dataset");
        add_path_option(bench_cmd, "--compiled,compiled", bench_config.compiled_path,
                        "Compiled dataset binary", true);
        bench_cmd->add_option("--batch-size,batch_size", bench_config.batch_size,
                              "Batch size to stream during the benchmark")
            ->type_name("INT");
        bench_cmd->add_option("--epochs,num_epochs", bench_epochs,
                              "Number of epochs to run during the benchmark")
            ->type_name("INT");

        std::string info_compiled_path;
        auto* info_cmd = app.add_subcommand(
            "info",
            "Inspect metadata from a compiled dataset binary");
        add_path_option(info_cmd, "--compiled,compiled", info_compiled_path,
                        "Compiled dataset binary", true);

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
