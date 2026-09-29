#pragma once
#include <filesystem>
#include "src/backend/imaging/resample/image_resize.h"
#include <cstdint>
#include <cstddef>
#include <string_view>
namespace mmltk::testsupport {
struct ExploreFixtureDimensions final {
 int source_width = 32;
 int source_height = 32;
 std::uint32_t compiled_width = 32U;
 std::uint32_t compiled_height = 32U;
 backend::imaging::resample::ImageResizeMode resize_mode = backend::imaging::resample::ImageResizeMode::Stretch;
};
struct ExploreFixtureAnnotations final {
 std::size_t objects = 0U;
 std::size_t runs_per_object = 0U;
 bool ring_and_dots = false;
 bool crowd_only = false;
 bool mixed_crowd = false;
 bool independent_masks = false;
 bool derive_boxes_from_masks = false;
};
[[nodiscard]] std::filesystem::path compile_explore_fixture(
 const std::filesystem::path& temporary_root, std::string_view fixture_name = "fixture", int num_images = 2, ExploreFixtureDimensions dimensions = {}, ExploreFixtureAnnotations annotations = {},
 bool pixel_evidence = false);
// The first ten source images are deliberately empty; image 10 carries class
// zero and image 11 carries class one. This gives system integration tests a
// trusted compiled split whose filter rows are observably distinct.
[[nodiscard]] std::filesystem::path compile_explore_membership_fixture(const std::filesystem::path& temporary_root);
// Retain background slot 0 and annotated slot 65 without moving their pixels
// or labels. Additional physical slots exercise trailing inactive storage.
[[nodiscard]] std::filesystem::path compile_explore_sparse_fixture(
 const std::filesystem::path& temporary_root, std::uint32_t slot_count = 66U, ExploreFixtureDimensions dimensions = {});
[[nodiscard]] std::filesystem::path corrupt_explore_label_index(const std::filesystem::path& source, const std::filesystem::path& destination);
}  // namespace mmltk::testsupport
