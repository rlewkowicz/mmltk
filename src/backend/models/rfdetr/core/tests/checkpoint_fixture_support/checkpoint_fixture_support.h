#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
namespace mmltk::backend::models::rfdetr::testsupport {

struct ParityFixtureCase;

void write_minimal_upstream_checkpoint(const std::filesystem::path& path, const ParityFixtureCase& fixture);
void log_fixture_phase(const char* test_name, std::size_t index, std::size_t total, const char* phase, const char* preset_name);

}  // namespace mmltk::backend::models::rfdetr::testsupport
