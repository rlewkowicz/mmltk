#pragma once
#include <array>
#include <cstdint>
#include "src/backend/models/rfdetr/core/model_state.h"
namespace at {
class Tensor;
}
namespace mmltk::backend::models::rfdetr::testsupport {
inline constexpr std::int64_t kParityFixtureNumClasses = 91;
inline constexpr std::int64_t kParityFixtureHiddenDim = 256;
struct ParityFixtureCase {
 const char* preset_name;
 const char* upstream_filename;
 std::int64_t query_rows;
 std::int64_t input_size;
 float offset;
};
[[nodiscard]] const std::array<ParityFixtureCase, 2>& parity_fixture_cases() noexcept;
[[nodiscard]] at::Tensor make_fixture_image(const ParityFixtureCase&);
[[nodiscard]] at::Tensor make_fixture_query_feat(const ParityFixtureCase&);
[[nodiscard]] at::Tensor make_fixture_refpoint_embed(const ParityFixtureCase&);
[[nodiscard]] at::Tensor make_fixture_class_weight(const ParityFixtureCase&);
[[nodiscard]] at::Tensor make_fixture_class_bias(const ParityFixtureCase&);
[[nodiscard]] DecodedNativeModelState make_native_parity_fixture(const ParityFixtureCase&);
void assert_matches_native_parity_fixture(const DecodedNativeModelState&, const ParityFixtureCase&);
}  // namespace mmltk::backend::models::rfdetr::testsupport
