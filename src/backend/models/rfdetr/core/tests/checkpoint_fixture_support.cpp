#include "checkpoint_fixture_support/checkpoint_fixture_support.h"
#include <cstdio>
#include <vector>
#include "src/test_support/subprocess_test_utils.hpp"
#include "checkpoint_fixture_support/parity_fixture_support.h"
#include "src/backend/models/rfdetr/core/model_state.h"
#include "src/backend/models/rfdetr/core/model.h"
namespace mmltk::backend::models::rfdetr::testsupport {
torch::serialize::InputArchive reload_archive(torch::serialize::OutputArchive& output, const std::filesystem::path& path) {
 output.save_to(path.string());
 torch::serialize::InputArchive input;
 input.load_from(path.string());
 return input;
}
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
void write_minimal_upstream_checkpoint(const std::filesystem::path& path, const ParityFixtureCase& fixture) { save_upstream_python_checkpoint(path, make_minimal_upstream_checkpoint_state(fixture)); }
void write_checkpoint_envelope(const std::filesystem::path& source, const std::filesystem::path& destination, CheckpointEnvelope envelope, std::string_view metadata) {
 const auto result = mmltk::testsupport::run_subprocess_capture_output(
  {"/usr/bin/env", "-u", "LD_LIBRARY_PATH", "-u", "PYTHONPATH", MMLTK_RFDETR_PYTHON_EXECUTABLE, "-c", R"py(
import json, sys, torch
checkpoint = torch.load(sys.argv[1], map_location="cpu", weights_only=False)
model = checkpoint["model"]
arguments = checkpoint.get("args", {})
arguments.update(json.loads(sys.argv[4]))
envelope = int(sys.argv[3])
if envelope == 0:
    checkpoint["args"] = arguments
elif envelope == 1:
    checkpoint = model
elif envelope == 5:
    checkpoint = {"state_dict": model, "args": arguments}
else:
    prefix = "model._orig_mod." if envelope == 3 else "model."
    state = {prefix + name: tensor for name, tensor in model.items()}
    state["model._kp_active_mask"] = torch.zeros(0, 0, dtype=torch.bool)
    if envelope == 4:
        name = next(iter(model))
        state["model._orig_mod." + name] = model[name]
    checkpoint = {"state_dict": state, "hyper_parameters": arguments,
                  **({"class_layout": checkpoint["class_layout"]} if "class_layout" in checkpoint else {})}
torch.save(checkpoint, sys.argv[2])
)py",
   source.string(), destination.string(), std::to_string(static_cast<int>(envelope)), std::string(metadata)});
 if (result.exit_code != 0) throw std::runtime_error("checkpoint envelope fixture failed: " + result.output_text);
}
void log_fixture_phase(const char* test_name, const std::size_t index, const std::size_t total, const char* phase, const char* preset_name) {
 std::fprintf(stderr, "%s: %zu/%zu %s %s\n", test_name, index, total, phase, preset_name);
 std::fflush(stderr);
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
