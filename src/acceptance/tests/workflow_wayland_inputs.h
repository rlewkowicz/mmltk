#pragma once
#include <filesystem>
#include <cstdint>
#include <string_view>
#include "src/backend/data/tests/test_fixture.h"
namespace mmltk::controller::contracts {
struct GuiSettingsState;
}
namespace mmltk::testsupport {
// Complete real model and media inputs owned by the packaged workflow scenario.
// The surrounding temporary directory outlives its browser and training child.
class WorkflowWaylandInputs final {
public:
 explicit WorkflowWaylandInputs(const std::filesystem::path& root, bool keypoints = false);
 [[nodiscard]] const auto& fixture() const noexcept { return fixture_; }
 [[nodiscard]] const auto& alternate_model() const noexcept { return alternate_; }
 void VerifyCatalogTraining(const std::filesystem::path& session_root, const std::filesystem::path& output, const std::filesystem::path& checkpoint, std::string_view run_id, std::string_view preset) const;
 void Configure(mmltk::controller::contracts::GuiSettingsState&, const std::filesystem::path& output) const;

private:
 mmltk::backend::data::testsupport::FixtureSpec fixture_;
 std::filesystem::path weights_;
 std::filesystem::path video_;
 std::filesystem::path alternate_;
 bool keypoints_ = false;
};
// Only the freshly exported session artifact is mutable. Restore it before the
// surrounding retained fixture root is released, including exceptional exits.
class WorkflowArtifactMutation final {
public:
 WorkflowArtifactMutation(const std::filesystem::path& session_root, std::filesystem::path alternate);
 ~WorkflowArtifactMutation();
 void Apply(std::uint32_t index);
 [[nodiscard]] const auto& artifact() const noexcept { return artifact_; }
private:
 void Restore() noexcept;
 std::filesystem::path artifact_, backup_, alternate_;
 std::uint32_t next_ = 0;
 bool held_ = false;
 bool companion_ = false;
 bool replacement_companion_ = false;
};
}  // namespace mmltk::testsupport
