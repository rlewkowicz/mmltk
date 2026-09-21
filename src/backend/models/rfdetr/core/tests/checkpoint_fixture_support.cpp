#include "checkpoint_fixture_support/checkpoint_fixture_support.h"
#include <cstdio>
#include <vector>
#include "checkpoint_fixture_support/parity_fixture_support.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/model.h"
namespace mmltk::backend::models::rfdetr::testsupport {
std::vector<NormalizedModelStateEntry> make_minimal_upstream_checkpoint_state(const ParityFixtureCase& fixture) {
 return {
  {"query_feat.weight", make_fixture_query_feat(fixture)},
  {"refpoint_embed.weight", make_fixture_refpoint_embed(fixture)},
  {"class_embed.weight", make_fixture_class_weight(fixture)},
  {"class_embed.bias", make_fixture_class_bias(fixture)},
 };
}
void save_upstream_python_checkpoint(const std::filesystem::path& path, const std::vector<NormalizedModelStateEntry>& state) {
 DecodedNativeModelState checkpoint;
 set_synthetic_model_state(checkpoint, state);
 write_upstream_model_state(path, checkpoint);
}
void write_minimal_upstream_checkpoint(const std::filesystem::path& path, const ParityFixtureCase& fixture) {
 save_upstream_python_checkpoint(path, make_minimal_upstream_checkpoint_state(fixture));
}
void log_fixture_phase(const char* test_name, const std::size_t index, const std::size_t total, const char* phase, const char* preset_name) {
 std::fprintf(stderr, "%s: %zu/%zu %s %s\n", test_name, index, total, phase, preset_name);
 std::fflush(stderr);
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
