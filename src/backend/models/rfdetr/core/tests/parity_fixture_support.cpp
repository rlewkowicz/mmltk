#include "src/backend/models/rfdetr/core/class_layout.h"
#include "checkpoint_fixture_support/parity_fixture_support.h"

#include <stdexcept>

#include "model_state_access.h"
#include "model_state_technical.h"
#include "model_technical.h"
#include "src/backend/models/rfdetr/core/model_state.h"

namespace mmltk::backend::models::rfdetr::testsupport {
namespace torch_api = mmltk::backend::ml::torch_api;

const std::array<ParityFixtureCase, 2>& parity_fixture_cases() noexcept {
    static const std::array<ParityFixtureCase, 2> cases{{
        {"rf-detr-nano", "rf-detr-nano.pth", std::int64_t{300} * 13, 32, 0.0F},
        {"rf-detr-seg-nano", "rf-detr-seg-nano.pt", std::int64_t{100} * 13, 24, 0.25F},
    }};
    return cases;
}
at::Tensor make_fixture_image(const ParityFixtureCase& fixture) {
    return torch_api::linspace(-1.0F + fixture.offset, 1.0F + fixture.offset, 3 * fixture.input_size * fixture.input_size,
                               torch_api::TensorOptions().dtype(torch_api::kFloat32))
        .reshape({3, fixture.input_size, fixture.input_size});
}
at::Tensor make_fixture_query_feat(const ParityFixtureCase& fixture) {
    return torch_api::arange(fixture.query_rows * kParityFixtureHiddenDim, torch_api::TensorOptions().dtype(torch_api::kFloat32))
        .reshape({fixture.query_rows, kParityFixtureHiddenDim})
        .mul(0.0005F)
        .add(fixture.offset);
}
at::Tensor make_fixture_refpoint_embed(const ParityFixtureCase& fixture) {
    return torch_api::linspace(-1.0F + fixture.offset, 1.0F + fixture.offset, fixture.query_rows * 4,
                               torch_api::TensorOptions().dtype(torch_api::kFloat32))
        .reshape({fixture.query_rows, 4});
}
at::Tensor make_fixture_class_weight(const ParityFixtureCase& fixture) {
    return torch_api::arange(kParityFixtureNumClasses * kParityFixtureHiddenDim, torch_api::TensorOptions().dtype(torch_api::kFloat32))
        .reshape({kParityFixtureNumClasses, kParityFixtureHiddenDim})
        .mul(0.00025F)
        .add(0.5F + fixture.offset);
}
at::Tensor make_fixture_class_bias(const ParityFixtureCase& fixture) {
    return torch_api::linspace(-1.0F + fixture.offset, 1.0F + fixture.offset, kParityFixtureNumClasses,
                               torch_api::TensorOptions().dtype(torch_api::kFloat32));
}
DecodedNativeModelState make_native_parity_fixture(const ParityFixtureCase& fixture) {
    DecodedNativeModelState checkpoint;
    checkpoint.metadata.preset_name = fixture.preset_name;
    checkpoint.metadata.source_kind = "golden-fixture";
    checkpoint.metadata.source_path = fixture.upstream_filename;
    checkpoint.metadata.num_classes = kParityFixtureNumClasses;
    checkpoint.metadata.class_layout = unresolved_class_layout(kParityFixtureNumClasses);
    checkpoint.metadata.num_queries = fixture.query_rows / 13;
    checkpoint.metadata.num_select = fixture.query_rows / 13;
    auto& entries = detail::model_state_owner(checkpoint).entries;
    entries.push_back({"query_feat.weight", make_fixture_query_feat(fixture)});
    entries.push_back({"refpoint_embed.weight", make_fixture_refpoint_embed(fixture)});
    entries.push_back({"class_embed.weight", make_fixture_class_weight(fixture)});
    entries.push_back({"class_embed.bias", make_fixture_class_bias(fixture)});
    return checkpoint;
}
void assert_matches_native_parity_fixture(const DecodedNativeModelState& checkpoint, const ParityFixtureCase& fixture) {
    const auto require = [](bool condition, const char* message) {
        if (!condition) throw std::runtime_error(message);
    };
    require(checkpoint.metadata.preset_name == fixture.preset_name, "fixture preset mismatch");
    require(checkpoint.metadata.source_kind == "golden-fixture", "fixture source kind mismatch");
    require(checkpoint.metadata.source_path == fixture.upstream_filename, "fixture source path mismatch");
    require(checkpoint.metadata.num_classes == kParityFixtureNumClasses, "fixture class count mismatch");
    require(checkpoint.metadata.class_layout == unresolved_class_layout(kParityFixtureNumClasses), "fixture class layout mismatch");
    require(checkpoint.metadata.num_queries == fixture.query_rows / 13, "fixture query count mismatch");
    const auto& entries = detail::model_state_owner(checkpoint).entries;
    require(entries.size() == 4, "fixture state_dict size mismatch");
    std::array<bool, 4> found{};
    for (const auto& entry : entries) {
        if (entry.name == "query_feat.weight") {
            found[0] = true;
            require(torch_api::equal(entry.tensor, make_fixture_query_feat(fixture)), "query_feat mismatch");
        } else if (entry.name == "refpoint_embed.weight") {
            found[1] = true;
            require(torch_api::equal(entry.tensor, make_fixture_refpoint_embed(fixture)), "refpoint mismatch");
        } else if (entry.name == "class_embed.weight") {
            found[2] = true;
            require(torch_api::equal(entry.tensor, make_fixture_class_weight(fixture)), "class weight mismatch");
        } else if (entry.name == "class_embed.bias") {
            found[3] = true;
            require(torch_api::equal(entry.tensor, make_fixture_class_bias(fixture)), "class bias mismatch");
        }
    }
    require(found[0] && found[1] && found[2] && found[3], "fixture state entry missing");
}
}  // namespace mmltk::backend::models::rfdetr::testsupport
