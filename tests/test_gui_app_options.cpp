#include "gui/app_options.h"

#include <cassert>

namespace {

using namespace fastloader::gui;

void test_flag_wins_over_env() {
    assert(resolve_vast_api_key("flag-key", "env-key") == "flag-key");
}

void test_env_fallback() {
    assert(resolve_vast_api_key("", "env-key") == "env-key");
}

void test_missing_values_resolve_empty() {
    assert(resolve_vast_api_key("", nullptr).empty());
}

} // namespace

int main() {
    test_flag_wins_over_env();
    test_env_fallback();
    test_missing_values_resolve_empty();
    return 0;
}
