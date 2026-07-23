#include "rfdetr/cli.h"
#include "cli_bootstrap.h"
#include "rfdetr/cli/workflow_cli_shared.h"
#include "distributed_train_launcher.h"

#include "cpu_affinity.h"
#include "execution_policy.h"
#include "mmltk/rfdetr/checkpoint.h"
#include "mmltk/rfdetr/model.h"
#include "mmltk/rfdetr/predict.h"
#include "mmltk/rfdetr/train.h"
#include "mmltk/rfdetr/validate.h"
#include "mmltk/rfdetr/workflow_requests.h"
#include "rfdetr/checkpoint_internal.h"
#include "rfdetr/tool_launch_utils.h"
#include "rfdetr/backends.h"
#include "dataset_compiler.h"
#include "compile_progress_monitor.h"
#include "mmltk_logging.h"
#include "spdmon/spdmon.hpp"
#include "CLI11.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mmltk::rfdetr {

class RfdetrCliRegistrar final {
   public:
    int run(int argc, char** argv) const;
};

namespace {

constexpr int kCliParseSuccess = static_cast<int>(CLI::ExitCodes::Success);

struct InfoOptions {
    std::filesystem::path onnx_path;
    std::filesystem::path tensorrt_path;
    int device_id = 0;
};

struct EvaluateCliOptions : EvaluateOptions {};

struct EvaluateCliState {
    EvaluateCliOptions options;
    std::string compile_mode = "selective";
};

struct NormalizeWeightsOptions {
    std::filesystem::path input_path;
    std::filesystem::path output_path;
};

struct CompileCliOptions {
    std::filesystem::path source_dir;
    std::filesystem::path output_dir;
    int resolution = 432;
    int num_workers = -1;
    int cuda_mask_batch_size = 0;
    int cuda_device_id = 0;
};

struct RegisteredRfdetrCli {
    mmltk::cli_support::LoggingOptions logging_options;
    CompileCliOptions compile_options;
    InfoOptions info_options;
    BuildEngineRequest build_engine_request;
    ExportOnnxRequest export_onnx_request;
    cli_shared::PredictCommandSpec predict_state;
    EvaluateCliState evaluate_state;
    cli_shared::TrainCommandSpec train_state;
    NormalizeWeightsOptions normalize_options;
    cli_shared::ValidateCommandSpec validate_state;
    CLI::App* compile_cmd = nullptr;
    CLI::App* info_cmd = nullptr;
    CLI::App* build_engine_cmd = nullptr;
    CLI::App* export_onnx_cmd = nullptr;
    CLI::App* predict_cmd = nullptr;
    CLI::App* evaluate_cmd = nullptr;
    CLI::App* train_cmd = nullptr;
    CLI::App* normalize_cmd = nullptr;
    CLI::App* validate_cmd = nullptr;
};

int handle_parse_error(CLI::App& app, const CLI::ParseError& error) {
    app.exit(error);
    return error.get_exit_code() == kCliParseSuccess ? 0 : 1;
}

[[noreturn]] void exec_onnx_info_tool(const std::filesystem::path& model_path) {
    const std::filesystem::path tool_path = resolve_sibling_tool_path("mmltk-rfdetr-onnx-info");
    const std::string tool = tool_path.string();
    const std::string model = model_path.string();
    std::vector<std::string> args{tool, model};
    std::vector<char*> raw_args = make_exec_argv(args);
    ::execv(raw_args.front(), raw_args.data());
    throw std::runtime_error("failed to exec ONNX info helper " + tool + ": " + std::strerror(errno));
}

void finalize_compile_options(const CompileCliOptions& options) {
    if (options.source_dir.empty() || options.output_dir.empty()) {
        throw std::runtime_error("rfdetr compile requires --source-dir and --output-dir");
    }
    if (options.cuda_mask_batch_size < 0) {
        throw std::runtime_error("rfdetr compile --cuda-mask-batch-size must be non-negative");
    }
}

void finalize_info_options(const InfoOptions& options) {
    cli_shared::require_exactly_one_model_input(options.onnx_path, options.tensorrt_path, "info");
}

void finalize_evaluate_options(EvaluateCliState& state) {
    state.options.compilation_mode = cli_shared::parse_compilation_mode(state.compile_mode);
    if (state.options.compiled_path.empty()) {
        throw std::runtime_error("rfdetr evaluate requires --compiled");
    }
    cli_shared::require_exactly_one_model_input(state.options, "evaluate");
}

void finalize_normalize_options(NormalizeWeightsOptions& options) {
    if (options.input_path.empty() || options.output_path.empty()) {
        throw std::runtime_error("rfdetr normalize-weights requires --input and --output");
    }
    options.input_path = detail::canonical_checkpoint_path(options.input_path);
    options.output_path = detail::canonical_checkpoint_path(options.output_path);
}

void run_compile(const CompileCliOptions& options) {
    CompilerConfig config;
    config.source_dir = options.source_dir.string();
    config.output_dir = options.output_dir.string();
    config.target_width = static_cast<uint32_t>(options.resolution);
    config.target_height = static_cast<uint32_t>(options.resolution);
    config.num_workers = options.num_workers;
    config.cuda_mask_batch_size = options.cuda_mask_batch_size;
    config.cuda_device_id = options.cuda_device_id;
    const DatasetCompilePlan plan = DatasetCompiler::prepare(config, {"train", "val"});

    for (size_t split_index = 0; split_index < plan.splits.size(); ++split_index) {
        const std::string& split = plan.splits[split_index].split;

        size_t last_done = 0;
        size_t total = 0;
        size_t progress_pulse = 0;
        spdmon::ProgressBar bar("compile " + split, 0, "img");
        compile_with_progress(plan, split_index,
                              [&bar, &last_done, &total, &progress_pulse](const CompileProgress& progress) {
                                  if (progress.total != total) {
                                      total = progress.total;
                                      bar.set_total(total);
                                  }
                                  if (progress.done > last_done) {
                                      bar.add(progress.done - last_done);
                                      last_done = progress.done;
                                  }
                                  bar.set_postfix(spdmon::format_compile_postfix(progress, progress_pulse++));
                              });
        bar.close();
    }
}

void configure_rfdetr_cli_app(CLI::App& app, RegisteredRfdetrCli& cli) {
    app.name("mmltk rfdetr");
    app.option_defaults()->always_capture_default();
    app.require_subcommand(1);
    mmltk::cli_support::add_logging_options(app, cli.logging_options);
    app.footer(
        "notes:\n"
        "  weights overlap mode requires --batch-size 1 when predict/evaluate --weights uses --lanes > 1\n"
        "  native train --lanes accepts any non-negative value; with --lanes > 1, effective batch per rank is "
        "batch_size * lanes * grad_accum_steps");
}

void register_rfdetr_commands(CLI::App& app, RegisteredRfdetrCli& cli) {
    cli.compile_cmd = app.add_subcommand("compile", "Compile train and val splits for RF-DETR experiments");
    auto* compile_dataset = cli.compile_cmd->add_option_group("Dataset");
    cli_shared::add_path_option(compile_dataset, "--source-dir", cli.compile_options.source_dir, "Source dataset root");
    cli_shared::add_path_option(compile_dataset, "--output-dir", cli.compile_options.output_dir,
                                "Output directory for compiled train/val binaries");
    compile_dataset
        ->add_option("--resolution", cli.compile_options.resolution, "Square image resolution for both splits")
        ->type_name("INT");
    compile_dataset
        ->add_option("--workers", cli.compile_options.num_workers,
                     "Total CPU worker budget for compile; 0 or negative selects all available CPUs")
        ->type_name("INT");
    compile_dataset
        ->add_option("--cuda-mask-batch-size", cli.compile_options.cuda_mask_batch_size,
                     "Batched CUDA mask-resize task count; 0 disables GPU mask resizing")
        ->type_name("INT");
    compile_dataset
        ->add_option("--cuda-device-id", cli.compile_options.cuda_device_id,
                     "CUDA device id used for batched mask resizing")
        ->type_name("INT");

    cli.info_cmd = app.add_subcommand("info", "Inspect ONNX or TensorRT model metadata");
    auto* info_model = cli.info_cmd->add_option_group("Model input");
    cli_shared::add_path_option(info_model, "--onnx", cli.info_options.onnx_path, "ONNX model path");
    cli_shared::add_path_option(info_model, "--tensorrt", cli.info_options.tensorrt_path,
                                "TensorRT engine path or ONNX path to build from");
    cli.info_cmd->add_option("--device-id", cli.info_options.device_id, "CUDA device id")->type_name("INT");

    cli.build_engine_cmd = app.add_subcommand("build-engine", "Build a TensorRT engine from an ONNX model");
    auto* build_engine_io = cli.build_engine_cmd->add_option_group("Input and output");
    auto* build_engine_exec = cli.build_engine_cmd->add_option_group("Execution");
    cli_shared::add_build_engine_request_options(build_engine_io, build_engine_exec, cli.build_engine_request);

    cli.export_onnx_cmd = app.add_subcommand("export-onnx", "Export .pt weights to ONNX format");
    auto* export_onnx_io = cli.export_onnx_cmd->add_option_group("Input and output");
    auto* export_onnx_exec = cli.export_onnx_cmd->add_option_group("Execution");
    cli_shared::add_export_onnx_request_options(export_onnx_io, export_onnx_exec, cli.export_onnx_request);

    cli.predict_cmd = app.add_subcommand("predict", "Run RF-DETR inference and write predictions to JSON");
    cli_shared::add_predict_command_options(cli.predict_cmd, cli.predict_state);

    cli.evaluate_cmd = app.add_subcommand("evaluate", "Run RF-DETR evaluation on a compiled dataset split");
    cli.evaluate_cmd->alias("eval");
    cli.evaluate_cmd->alias("val");
    auto* evaluate_dataset = cli.evaluate_cmd->add_option_group("Dataset");
    cli_shared::add_path_option(evaluate_dataset, "--compiled", cli.evaluate_state.options.compiled_path,
                                "Compiled dataset split (.bin)");
    auto* evaluate_model = cli.evaluate_cmd->add_option_group("Model input");
    cli_shared::add_model_input_options(evaluate_model, cli.evaluate_state.options,
                                        "TensorRT engine path or ONNX path to build from");
    auto* evaluate_execution = cli.evaluate_cmd->add_option_group("Execution");
    evaluate_execution->add_option("--batch-size", cli.evaluate_state.options.batch_size, "Batch size for evaluation")
        ->type_name("INT");
    evaluate_execution->add_option("--device-id", cli.evaluate_state.options.device_id, "CUDA device id")
        ->type_name("INT");
    evaluate_execution
        ->add_option("--limit-images", cli.evaluate_state.options.limit_images, "Limit the number of evaluated images")
        ->type_name("INT");
    evaluate_execution
        ->add_option("--eval-max-dets", cli.evaluate_state.options.eval_max_dets,
                     "Maximum detections per image during evaluation")
        ->type_name("INT");
    evaluate_execution->add_option("--workers", cli.evaluate_state.options.workers, "Dataset worker count")
        ->type_name("INT");
    evaluate_execution->add_option("--lanes", cli.evaluate_state.options.lanes, "Parallel backend lane count")
        ->type_name("INT");
    evaluate_execution->add_option("--cpu-affinity", cli.evaluate_state.options.cpu_affinity,
                                   "Linux CPU list or range string for workers");
    evaluate_execution->add_option("--backend", cli.evaluate_state.options.backend,
                                   "Backend preference for ONNX/TensorRT artifacts: auto, onnx, or tensorrt");
    evaluate_execution->add_flag("--fp16,!--no-fp16", cli.evaluate_state.options.allow_fp16,
                                 "Enable BF16 weights evaluation or supported FP16 fallback/backend execution");
    evaluate_execution->add_flag("--progress,!--no-progress", cli.evaluate_state.options.progress_bar,
                                 "Render interactive progress output");
    cli_shared::add_compile_mode_option(evaluate_execution, cli.evaluate_state.compile_mode);

    cli.train_cmd = app.add_subcommand("train", "Train RF-DETR natively against compiled datasets");
    cli_shared::add_train_command_options(cli.train_cmd, cli.train_state);

    cli.normalize_cmd =
        app.add_subcommand("normalize-weights", "Convert an upstream RF-DETR checkpoint into mmltk's native format");
    auto* normalize_io = cli.normalize_cmd->add_option_group("Input and output");
    cli_shared::add_path_option(normalize_io, "--input", cli.normalize_options.input_path,
                                "Input upstream checkpoint path");
    cli_shared::add_path_option(normalize_io, "--output", cli.normalize_options.output_path,
                                "Output native checkpoint path");

    cli.validate_cmd =
        app.add_subcommand("validate", "Compare ONNX and TensorRT backends against a compiled dataset split");
    cli_shared::add_validate_command_options(cli.validate_cmd, cli.validate_state);
}

std::vector<std::string> build_rfdetr_parse_args(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(argc - 1));
    args.emplace_back("mmltk rfdetr");
    for (int index = 2; index < argc; ++index) {
        args.emplace_back(argv[index]);
    }
    return args;
}

std::vector<const char*> build_rfdetr_parse_argv(const std::vector<std::string>& args) {
    std::vector<const char*> raw_args;
    raw_args.reserve(args.size());
    for (const auto& arg : args) {
        raw_args.push_back(arg.c_str());
    }
    return raw_args;
}

bool parse_rfdetr_cli(CLI::App& app, int argc, char** argv, int* exit_code) {
    const std::vector<std::string> args = build_rfdetr_parse_args(argc, argv);
    std::vector<const char*> raw_args = build_rfdetr_parse_argv(args);
    try {
        app.parse(static_cast<int>(raw_args.size()), raw_args.data());
    } catch (const CLI::ParseError& error) {
        if (exit_code != nullptr) {
            *exit_code = handle_parse_error(app, error);
        }
        return false;
    }
    if (exit_code != nullptr) {
        *exit_code = kCliParseSuccess;
    }
    return true;
}

int run_compile_command(const CompileCliOptions& options) {
    finalize_compile_options(options);
    run_compile(options);
    return 0;
}

int run_info_command(const InfoOptions& options) {
    finalize_info_options(options);
    if (!options.onnx_path.empty()) {
        exec_onnx_info_tool(options.onnx_path);
    }
    std::unique_ptr<InferenceBackend> backend = make_tensorrt_backend(options.tensorrt_path, options.device_id, true);
    print_model_metadata(backend->info(), 0, 0, ValidationLogMode::Interactive);
    return 0;
}

int run_build_engine_command(const BuildEngineRequest& request) {
    validate_build_engine_request(request);
    make_tensorrt_backend(request.onnx_path, request.device_id, request.allow_fp16, request.output_path);
    mmltk::logging::logger("rfdetr.cli")->info("tensorrt: wrote engine to {}", request.output_path.string());
    return 0;
}

int run_export_onnx_command(const ExportOnnxRequest& request) {
    validate_export_onnx_request(request);
    export_weights_to_onnx(request, request.output_path, request.device_id, request.opset_version, request.simplify);
    return 0;
}

int run_predict_command(cli_shared::PredictCommandSpec& state) {
    cli_shared::finalize_predict_command(state);
    const PredictOptions options = to_predict_options(state.request);
    const PredictionRunResult result = run_prediction(options);
    write_prediction_json(options, result);
    print_prediction_summary(options, result);
    return 0;
}

int run_evaluate_command(EvaluateCliState& state) {
    finalize_evaluate_options(state);
    const EvaluationRunResult result = run_evaluation(state.options);
    print_evaluation_summary(state.options, result);
    return 0;
}

int run_train_command(cli_shared::TrainCommandSpec& state, int argc, char** argv) {
    cli_shared::finalize_train_command(state);
    const TrainOptions options = to_train_options(state.request);
    if (!options.distributed_worker && options.device_ids.size() > 1) {
        return cli_support::spawn_distributed_training_workers(options, argc, argv);
    }
    const TrainRunResult result = run_training(options);
    print_training_summary(options, result);
    return 0;
}

int run_normalize_command(NormalizeWeightsOptions& options) {
    finalize_normalize_options(options);
    const NativeCheckpoint checkpoint = normalize_checkpoint_to_native(options.input_path, options.output_path);
    mmltk::logging::logger("rfdetr.cli")
        ->info("rfdetr.normalize-weights: wrote {} tensors for preset={} to {}", checkpoint.state_dict.size(),
               checkpoint.metadata.preset_name, options.output_path.string());
    return 0;
}

int run_validate_command(cli_shared::ValidateCommandSpec& state) {
    cli_shared::finalize_validate_command(state);
    const ValidationOptions options = to_validate_options(state.request);
    const ValidationRunResult result = run_validation(options);
    write_validation_report(options, result);
    print_validation_run_summary(options, result);
    return 0;
}

int execute_rfdetr_cli_command(RegisteredRfdetrCli& cli, int argc, char** argv) {
    if (cli.compile_cmd->parsed()) {
        return run_compile_command(cli.compile_options);
    }

    if (cli.info_cmd->parsed()) {
        return run_info_command(cli.info_options);
    }

    if (cli.build_engine_cmd->parsed()) {
        return run_build_engine_command(cli.build_engine_request);
    }

    if (cli.export_onnx_cmd->parsed()) {
        return run_export_onnx_command(cli.export_onnx_request);
    }

    if (cli.predict_cmd->parsed()) {
        return run_predict_command(cli.predict_state);
    }

    if (cli.evaluate_cmd->parsed()) {
        return run_evaluate_command(cli.evaluate_state);
    }

    if (cli.train_cmd->parsed()) {
        return run_train_command(cli.train_state, argc, argv);
    }

    if (cli.normalize_cmd->parsed()) {
        return run_normalize_command(cli.normalize_options);
    }

    if (cli.validate_cmd->parsed()) {
        return run_validate_command(cli.validate_state);
    }

    return 1;
}

}  

int RfdetrCliRegistrar::run(int argc, char** argv) const {
    if (argc < 2 || std::string(argv[1]) != "rfdetr") {
        return -1;
    }
    try {
        CLI::App app{"RF-DETR model tooling for compilation, inference, evaluation, and training"};
        RegisteredRfdetrCli cli;
        configure_rfdetr_cli_app(app, cli);
        register_rfdetr_commands(app, cli);
        int parse_exit_code = kCliParseSuccess;
        if (!parse_rfdetr_cli(app, argc, argv, &parse_exit_code)) {
            return parse_exit_code;
        }
        return execute_rfdetr_cli_command(cli, argc, argv);
    } catch (const std::exception& error) {
        mmltk::logging::logger("rfdetr.cli")->error("mmltk rfdetr error: {}", error.what());
        return 1;
    }
}

int handle_cli(int argc, char** argv) {
    RfdetrCliRegistrar registrar;
    return registrar.run(argc, argv);
}

}  
