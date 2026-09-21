#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/data/dataset_compiler.h"
#include <stb_image_write.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
namespace mmltk::backend::data::testsupport {
namespace {
std::vector<uint8_t> stub_rgb_pixels(const std::string& path, int width, int height) {
 std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
 uint32_t hash = 0;
 for (char c : path) { hash = hash * 31u + static_cast<uint8_t>(c); }
 for (size_t i = 0; i < pixels.size(); ++i) { pixels[i] = static_cast<uint8_t>((hash + i * 7u) & 0xFFu); }
 return pixels;
}
void write_png_stub(const std::string& path, int width, int height, bool pixel_evidence) {
 std::vector<uint8_t> pixels = stub_rgb_pixels(path, width, height);
 if (pixel_evidence) {
  for (std::size_t index = 0; index < pixels.size(); index += 3U) {
   pixels[index] = 48U;
   pixels[index + 1U] = 80U;
   pixels[index + 2U] = 112U;
  }
 }
 stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3);
}
constexpr std::array<const char*, 6> kClassNames{
 "person", "ret", "scope", "iron_sight", "anchor_dot", "glint",
};
void write_synthetic_sample(const fs::path& split_dir, const int image_index, const int width, const int height, const int background_images,
                            const bool pixel_evidence) {
 std::array<char, 64> fname{};
 std::snprintf(fname.data(), fname.size(), "%06d.png", image_index);
 write_png_stub((split_dir / fname.data()).string(), width, height, pixel_evidence);
 std::snprintf(fname.data(), fname.size(), "%06d.jsonl", image_index);
 std::ofstream annotations(split_dir / fname.data(), std::ios::trunc);
 if (image_index <= background_images) return;
 const int cls = (image_index - background_images - 1) % static_cast<int>(kClassNames.size());
 const int x1 = pixel_evidence ? width / 4 : 10;
 const int y1 = pixel_evidence ? height / 4 : 10;
 const int x2 = pixel_evidence ? width * 3 / 4 : std::min(width - 1, 30);
 const int y2 = pixel_evidence ? height * 3 / 4 : std::min(height - 1, 30);
 std::string rle;
 for (int row = y1; row < y2; ++row) {
  const auto append_run = [&](int first, int last) {
   if (!rle.empty()) rle += " ";
   rle += std::to_string(row * width + first) + ":" + std::to_string(last - first);
  };
  if (pixel_evidence && row >= height * 7 / 16 && row < height * 9 / 16) {
   append_run(x1, width * 7 / 16);
   append_run(width * 9 / 16, x2);
  } else {
   append_run(x1, x2);
  }
 }
 annotations << R"({"class":")" << kClassNames[cls] << R"(","bbox_xyxy":[)" << x1 << "," << y1 << "," << x2 << "," << y2
             << R"(],"mask_rle_encoding":"row_major_start_length","mask_rle":")" << rle << R"(","image_size_wh":[)" << width << "," << height << R"(]})"
             << "\n";
}
}  // namespace
std::string dataset_dir(const FixtureSpec& spec) { return spec.root_dir + "/dataset"; }
std::string compiled_dir(const FixtureSpec& spec) { return spec.root_dir + "/compiled"; }
std::string compiled_bin_path(const FixtureSpec& spec) { return compiled_dir(spec) + "/" + spec.split + ".bin"; }
CompilerConfig compiler_config(const FixtureSpec& spec) {
 CompilerConfig config;
 config.source_dir = dataset_dir(spec);
 config.output_dir = compiled_dir(spec);
 config.split = spec.split;
 config.target_width = spec.width;
 config.target_height = spec.height;
 return config;
}
void compile_existing_fixture(const FixtureSpec& spec) {
 auto config = compiler_config(spec);
 config.num_workers = 1;
 const auto plan = DatasetCompiler::prepare(config, {config.split});
 DatasetCompiler::compile(plan, 0U);
}
void create_synthetic_dataset(const FixtureSpec& spec) {
 const std::string data_dir = dataset_dir(spec);
 const fs::path split_dir = fs::path(data_dir) / spec.split;
 std::error_code remove_error;
 fs::remove_all(spec.root_dir, remove_error);
 if (remove_error) { throw std::runtime_error("failed to clear synthetic dataset root: " + remove_error.message()); }
 fs::create_directories(split_dir);
 const int background_images = std::clamp(spec.background_images, 0, spec.num_images);
 const int annotated_images = std::max(spec.num_images - background_images, 0);
 {
  std::ofstream f(data_dir + "/categories.json");
  f << "{\n"
    << R"(  "meta": {"dataset_name":"test","version":"1.0","image_format":"png",
)" << R"(           "image_size_wh":[)"
    << spec.width << "," << spec.height << R"(],"bbox_format":"xyxy_absolute_pixels",
)" << R"(           "mask_format":"rle_row_major_start_length",
)" << R"(           "background_annotation_policy":"empty_jsonl_file"},
)" << R"(  "classes": [
)" << R"(    {"id":)"
    << spec.first_class_id << R"(,"name":"person"},)" << R"({"id":)" << spec.first_class_id + 1 << R"(,"name":"ret"},)" << R"({"id":)"
    << spec.first_class_id + 2 << R"(,"name":"scope"},
)" << R"(    {"id":)"
    << spec.first_class_id + 3 << R"(,"name":"iron_sight"},)" << R"({"id":)" << spec.first_class_id + 4 << R"(,"name":"anchor_dot"},)"
    << R"({"id":)" << spec.first_class_id + 5 << R"(,"name":"glint"}
)" << R"(  ],
)" << R"json(  "splits": {")json"
    << spec.split << R"json(":{"total":)json" << spec.num_images << R"(,"background":)" << background_images << R"(,"annotated":)" << annotated_images << R"(}}
)" << "}";
 }
 for (int i = 1; i <= spec.num_images; ++i) { write_synthetic_sample(split_dir, i, spec.width, spec.height, background_images, spec.pixel_evidence); }
}
void replace_synthetic_image(const FixtureSpec& spec, const int image_index, const int width, const int height) {
 if (image_index < 1 || image_index > spec.num_images || width <= 30 || height <= 30) throw std::invalid_argument("synthetic replacement image is invalid");
 write_synthetic_sample(fs::path(dataset_dir(spec)) / spec.split, image_index, width, height, std::clamp(spec.background_images, 0, spec.num_images),
                        spec.pixel_evidence);
}
std::vector<float> expected_nchw_stub(const std::string& path, int width, int height) {
 const std::vector<uint8_t> pixels = stub_rgb_pixels(path, width, height);
 const size_t hw = static_cast<size_t>(width) * height;
 std::vector<float> nchw(size_t{3} * hw);
 for (size_t i = 0; i < hw; ++i) {
  nchw[i] = static_cast<float>(pixels[i * 3 + 0]) / 255.0f;
  nchw[hw + i] = static_cast<float>(pixels[i * 3 + 1]) / 255.0f;
  nchw[2 * hw + i] = static_cast<float>(pixels[i * 3 + 2]) / 255.0f;
 }
 return nchw;
}
std::vector<float> expected_resized_rgb(std::span<const std::uint8_t> rgb, const std::uint32_t width, const std::uint32_t height,
                                        const std::uint32_t target_width, const std::uint32_t target_height,
                                        const mmltk::backend::imaging::resample::ImageResizeMode mode, const bool perceptual) {
 using namespace mmltk::backend::imaging::resample;
 if (rgb.size() != std::size_t(width) * height * 3U) throw std::invalid_argument("RGB fixture extent mismatch");
 const auto geometry = compute_image_resize_geometry(width, height, target_width, target_height, mode);
 RgbImageResizer resizer(1, perceptual);
 std::vector<std::uint8_t> bytes(std::size_t(geometry.resized_width) * geometry.resized_height * 3U);
 resizer.resize(rgb.data(), static_cast<int>(width), static_cast<int>(height), bytes.data(), static_cast<int>(geometry.resized_width),
                static_cast<int>(geometry.resized_height));
 std::vector<float> expected(std::size_t(target_width) * target_height * 3U);
 letterboxed_rgb_hwc_u8_to_nchw_f32(bytes.data(), expected.data(), geometry.resized_width, geometry.resized_height, target_width, target_height,
                                    geometry.offset_x, geometry.offset_y);
 return expected;
}
void assert_image_matches(const float* actual, const std::vector<float>& expected) {
 if (expected.size() <= 8) { throw std::runtime_error("expected image fixture must contain more than eight samples"); }
 for (size_t i = 0; i < expected.size(); ++i) {
  if (std::fabs(actual[i] - expected[i]) >= 1e-6f) { throw std::runtime_error("fixture image mismatch at index " + std::to_string(i)); }
 }
}
}  // namespace mmltk::backend::data::testsupport
