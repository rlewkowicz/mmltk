#pragma once
#include <concepts>
#include <filesystem>
#include <utility>
namespace mmltk::controller::shell::testsupport {
template <std::invocable Factory>
[[nodiscard]] auto make_train_settings(const std::filesystem::path& source, const std::filesystem::path& compiled, Factory&& factory) {
    auto settings = std::forward<Factory>(factory)();
    auto& train = settings.workflows.train;
    train.dataset_source_dir = source.string();
    train.compiled_dataset_dir = compiled.string();
    train.overwrite_compiled_dataset = true;
    train.request.train_compiled_path = compiled / "train.bin";
    train.request.val_compiled_path = compiled / "val.bin";
    train.request.output_dir = compiled / "train-output";
    train.request.resolution = 16;
    return settings;
}
}  // namespace mmltk::controller::shell::testsupport
