#pragma once
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/imaging/resample/image_resize.h"
#include <span>
#include <cstdint>
#include <string>
#include <vector>
namespace mmltk::backend::data::testsupport {
struct FixtureSpec {
    std::string root_dir;
    std::string split = "train";
    int width = 65;
    int height = 65;
    int num_images = 20;
    int first_class_id = 1;
    int background_images = 10;
    bool pixel_evidence = false;
};
std::string dataset_dir(const FixtureSpec& spec);
std::string compiled_dir(const FixtureSpec& spec);
std::string compiled_bin_path(const FixtureSpec& spec);
void create_synthetic_dataset(const FixtureSpec& spec);
[[nodiscard]] CompilerConfig compiler_config(const FixtureSpec& spec);
void compile_existing_fixture(const FixtureSpec& spec);
void replace_synthetic_image(const FixtureSpec& spec, int image_index, int width, int height);
std::vector<float> expected_nchw_stub(const std::string& path, int width, int height);
// Retained compiler algorithm: RGB8 resize, then the public planar converter.
std::vector<float> expected_resized_rgb(std::span<const std::uint8_t> rgb, std::uint32_t width, std::uint32_t height,
                                       std::uint32_t target_width, std::uint32_t target_height,
                                       mmltk::backend::imaging::resample::ImageResizeMode mode, bool perceptual);
void assert_image_matches(const float* actual, const std::vector<float>& expected);
}  // namespace mmltk::backend::data::testsupport
