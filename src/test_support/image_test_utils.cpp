#include "src/test_support/image_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
namespace mmltk::testsupport {
RgbaTestImage read_rgba_image(const std::filesystem::path& path) {
 RgbaTestImage result;
 int channels = 0;
 result.pixels.reset(stbi_load(path.c_str(), &result.width, &result.height, &channels, 4));
 REQUIRE(result.pixels);
 return result;
}
}  // namespace mmltk::testsupport
