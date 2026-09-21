#pragma once
#include <filesystem>
#include "src/backend/data/tests/test_fixture.h"
namespace mmltk::controller::contracts {
struct GuiSettingsState;
}
namespace mmltk::testsupport {
// Complete real model and media inputs owned by the packaged workflow scenario.
// The surrounding temporary directory outlives its browser and training child.
class WorkflowWaylandInputs final {
public:
 explicit WorkflowWaylandInputs(const std::filesystem::path& root);
 void Configure(mmltk::controller::contracts::GuiSettingsState&, const std::filesystem::path& output) const;

private:
 mmltk::backend::data::testsupport::FixtureSpec fixture_;
 std::filesystem::path weights_;
 std::filesystem::path video_;
};
}  // namespace mmltk::testsupport
