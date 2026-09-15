#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include "src/backend/models/rfdetr/core/class_layout.h"
namespace mmltk::backend::models::rfdetr::testsupport {

struct ParityFixtureCase;
// Synthetic fixture vocabulary is explicit test data, never artifact inference.
[[nodiscard]] inline ModelClassLayout synthetic_training_layout(std::size_t foreground_count) {
    std::vector<std::string> names;
    names.reserve(foreground_count);
    for (std::size_t index = 0; index < foreground_count; ++index) names.push_back("fixture-class-" + std::to_string(index));
    return native_training_class_layout(mmltk::backend::data::catalog::ClassCatalog(std::move(names)));
}

void write_minimal_upstream_checkpoint(const std::filesystem::path& path, const ParityFixtureCase& fixture);
void log_fixture_phase(const char* test_name, std::size_t index, std::size_t total, const char* phase, const char* preset_name);

}  // namespace mmltk::backend::models::rfdetr::testsupport
