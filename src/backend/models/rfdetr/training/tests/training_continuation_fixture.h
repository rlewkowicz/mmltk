#pragma once

#include <sstream>
#include <string_view>

#include "torch_api.h"

namespace mmltk::backend::models::rfdetr::testsupport {

// Preserve nested optimizer/state archives while changing one top-level fact.
inline void copy_checkpoint_archive(mmltk::backend::ml::torch_api::InputArchive& source,
                                    mmltk::backend::ml::torch_api::OutputArchive& destination,
                                    std::string_view omitted = {}) {
    namespace api = mmltk::backend::ml::torch_api;
    for (const auto& key : source.keys()) {
        if (key == omitted) continue;
        api::InputArchive child;
        if (source.try_read(key, child)) {
            api::OutputArchive output;
            copy_checkpoint_archive(child, output);
            destination.write(key, output);
        } else {
            api::IValue value;
            source.read(key, value);
            destination.write(key, value);
        }
    }
}

inline mmltk::backend::ml::torch_api::InputArchive checkpoint_input(
    mmltk::backend::ml::torch_api::OutputArchive& output) {
    std::stringstream bytes;
    output.save_to(bytes);
    mmltk::backend::ml::torch_api::InputArchive input;
    input.load_from(bytes, mmltk::backend::ml::torch_api::Device(mmltk::backend::ml::torch_api::kCPU));
    return input;
}

}  // namespace mmltk::backend::models::rfdetr::testsupport
