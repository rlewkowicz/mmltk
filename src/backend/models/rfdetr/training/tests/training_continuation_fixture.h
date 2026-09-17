#pragma once
#include <sstream>
#include <string_view>
#include <torch/types.h>
#include <torch/serialize.h>
namespace mmltk::backend::models::rfdetr::testsupport {
// Preserve nested optimizer/state archives while changing one top-level fact.
inline void copy_checkpoint_archive(torch::serialize::InputArchive& source, torch::serialize::OutputArchive& destination, std::string_view omitted = {}) {
    for (const auto& key : source.keys()) {
        if (key == omitted) continue;
        torch::serialize::InputArchive child;
        if (source.try_read(key, child)) {
            torch::serialize::OutputArchive output;
            copy_checkpoint_archive(child, output);
            destination.write(key, output);
        } else {
            c10::IValue value;
            source.read(key, value);
            destination.write(key, value);
        }
    }
}
inline torch::serialize::InputArchive checkpoint_input(torch::serialize::OutputArchive& output) {
    std::stringstream bytes;
    output.save_to(bytes);
    torch::serialize::InputArchive input;
    input.load_from(bytes, torch::Device(torch::kCPU));
    return input;
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
