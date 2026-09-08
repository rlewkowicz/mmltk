#include <catch2/catch_test_macros.hpp>

#include <type_traits>
#include <utility>

#include "src/controller/shell/application_shell.h"

namespace mmltk::controller::shell {
namespace {

TEST_CASE("application shell is a single non-copyable ordinary composition") {
    STATIC_CHECK(std::is_constructible_v<ApplicationShell, ApplicationShellConfig>);
    STATIC_CHECK_FALSE(std::is_copy_constructible_v<ApplicationShell>);
    STATIC_CHECK_FALSE(std::is_copy_assignable_v<ApplicationShell>);
}

TEST_CASE("shell visual wiring exposes exact-frame Upscale warm-up") {
    STATIC_CHECK(std::is_same_v<decltype(UpscaleRequest{}.source), VisualFrame>);
    STATIC_CHECK(std::is_same_v<decltype(UpscaleRequest{}.document), VisualDocumentFacts>);
    STATIC_CHECK(std::is_same_v<decltype(ExploreSnapshot{}.document), VisualDocumentFacts>);
    STATIC_CHECK(std::is_trivially_copyable_v<UpscaleRequest>);
    STATIC_CHECK(noexcept(std::declval<UpscaleSystem&>().Warm(VisualExtent{})));
    STATIC_CHECK(std::is_invocable_r_v<VisualDocumentRead, ExactVisualDocumentBorrower, const VisualFrame&>);
}

}  // namespace
}  // namespace mmltk::controller::shell
