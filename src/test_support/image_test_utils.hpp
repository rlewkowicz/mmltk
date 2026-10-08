#pragma once
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <memory>
#include <stb_image.h>
namespace mmltk::testsupport {
template <class Owner>
[[nodiscard]] auto require_borrowed_image(Owner& owner, std::optional<std::uint64_t> revision = {}) {
 auto image = owner.Borrow();
 REQUIRE(image.valid());
 if (revision) CHECK(image.plane(0U).revision() == *revision);
 return image;
}
struct RgbaTestImage final {
 int width = 0;
 int height = 0;
 std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{nullptr, stbi_image_free};
};
[[nodiscard]] RgbaTestImage read_rgba_image(const std::filesystem::path& path);
}  // namespace mmltk::testsupport
