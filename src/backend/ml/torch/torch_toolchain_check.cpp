#include <torch/version.h>
#include <string_view>
static_assert(!std::string_view{TORCH_VERSION}.empty());
