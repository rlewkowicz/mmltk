#pragma once

#include "mmltk/rfdetr/checkpoint.h"

#include <array>
#include <stdexcept>

namespace mmltk::rfdetr::testsupport {

inline constexpr int64_t kParityFixtureNumClasses = 91;
inline constexpr int64_t kParityFixtureHiddenDim = 256;

struct ParityFixtureCase {
    const char* preset_name;
    const char* upstream_filename;
    int64_t query_rows;
    int64_t input_size;
    float offset;
};

inline const std::array<ParityFixtureCase, 2>& parity_fixture_cases() {
    static const std::array<ParityFixtureCase, 2> cases{{
        {"rf-detr-nano", "rf-detr-nano.pth", int64_t{300} * int64_t{13}, 32, 0.0f},
        {"rf-detr-seg-nano", "rf-detr-seg-nano.pt", int64_t{100} * int64_t{13}, 24, 0.25f},
    }};
    return cases;
}

inline torch::Tensor make_fixture_image(const ParityFixtureCase& fixture) {
    return torch::linspace(
               -1.0f + fixture.offset,
               1.0f + fixture.offset,
               3 * fixture.input_size * fixture.input_size,
               torch::TensorOptions().dtype(torch::kFloat32))
        .reshape({3, fixture.input_size, fixture.input_size});
}

inline torch::Tensor make_fixture_query_feat(const ParityFixtureCase& fixture) {
    return torch::arange(
               fixture.query_rows * kParityFixtureHiddenDim,
               torch::TensorOptions().dtype(torch::kFloat32))
        .reshape({fixture.query_rows, kParityFixtureHiddenDim})
        .mul(0.0005f)
        .add(fixture.offset);
}

inline torch::Tensor make_fixture_refpoint_embed(const ParityFixtureCase& fixture) {
    return torch::linspace(
               -1.0f + fixture.offset,
               1.0f + fixture.offset,
               fixture.query_rows * 4,
               torch::TensorOptions().dtype(torch::kFloat32))
        .reshape({fixture.query_rows, 4});
}

inline torch::Tensor make_fixture_class_weight(const ParityFixtureCase& fixture) {
    return torch::arange(
               kParityFixtureNumClasses * kParityFixtureHiddenDim,
               torch::TensorOptions().dtype(torch::kFloat32))
        .reshape({kParityFixtureNumClasses, kParityFixtureHiddenDim})
        .mul(0.00025f)
        .add(0.5f + fixture.offset);
}

inline torch::Tensor make_fixture_class_bias(const ParityFixtureCase& fixture) {
    return torch::linspace(
        -1.0f + fixture.offset,
        1.0f + fixture.offset,
        kParityFixtureNumClasses,
        torch::TensorOptions().dtype(torch::kFloat32));
}

inline NativeCheckpoint make_native_parity_fixture(const ParityFixtureCase& fixture) {
    NativeCheckpoint checkpoint;
    checkpoint.metadata.preset_name = fixture.preset_name;
    checkpoint.metadata.source_kind = "golden-fixture";
    checkpoint.metadata.source_path = fixture.upstream_filename;
    checkpoint.metadata.num_classes = kParityFixtureNumClasses;
    checkpoint.state_dict.push_back({"query_feat.weight", make_fixture_query_feat(fixture)});
    checkpoint.state_dict.push_back({"refpoint_embed.weight", make_fixture_refpoint_embed(fixture)});
    checkpoint.state_dict.push_back({"class_embed.weight", make_fixture_class_weight(fixture)});
    checkpoint.state_dict.push_back({"class_embed.bias", make_fixture_class_bias(fixture)});
    return checkpoint;
}

inline void parity_fixture_require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline void assert_matches_native_parity_fixture(const NativeCheckpoint& checkpoint,
                                                 const ParityFixtureCase& fixture) {
    parity_fixture_require(checkpoint.metadata.preset_name == fixture.preset_name, "fixture preset mismatch");
    parity_fixture_require(checkpoint.metadata.source_kind == "golden-fixture", "fixture source kind mismatch");
    parity_fixture_require(checkpoint.metadata.source_path == fixture.upstream_filename, "fixture source path mismatch");
    parity_fixture_require(
        checkpoint.metadata.num_classes == kParityFixtureNumClasses,
        "fixture class count mismatch");
    parity_fixture_require(checkpoint.state_dict.size() == 4, "fixture state_dict size mismatch");

    bool found_query_feat = false;
    bool found_refpoint = false;
    bool found_class_weight = false;
    bool found_class_bias = false;
    for (const auto& entry : checkpoint.state_dict) {
        if (entry.name == "query_feat.weight") {
            found_query_feat = true;
            parity_fixture_require(torch::equal(entry.tensor, make_fixture_query_feat(fixture)), "query_feat mismatch");
        } else if (entry.name == "refpoint_embed.weight") {
            found_refpoint = true;
            parity_fixture_require(
                torch::equal(entry.tensor, make_fixture_refpoint_embed(fixture)),
                "refpoint_embed mismatch");
        } else if (entry.name == "class_embed.weight") {
            found_class_weight = true;
            parity_fixture_require(
                torch::equal(entry.tensor, make_fixture_class_weight(fixture)),
                "class_embed.weight mismatch");
        } else if (entry.name == "class_embed.bias") {
            found_class_bias = true;
            parity_fixture_require(
                torch::equal(entry.tensor, make_fixture_class_bias(fixture)),
                "class_embed.bias mismatch");
        }
    }

    parity_fixture_require(found_query_feat, "fixture query_feat entry missing");
    parity_fixture_require(found_refpoint, "fixture refpoint entry missing");
    parity_fixture_require(found_class_weight, "fixture class_embed.weight entry missing");
    parity_fixture_require(found_class_bias, "fixture class_embed.bias entry missing");
}

} // namespace mmltk::rfdetr::testsupport
