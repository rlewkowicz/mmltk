#include "stb_image_write.h"

#include "test_fixture.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace fastloader::testsupport {

namespace {

std::vector<uint8_t> stub_rgb_pixels(const std::string& path, int width, int height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 3);
    uint32_t hash = 0;
    for (char c : path) {
        hash = hash * 31u + static_cast<uint8_t>(c);
    }
    for (size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint8_t>((hash + i * 7u) & 0xFFu);
    }
    return pixels;
}

void write_png_stub(const std::string& path, int width, int height) {
    std::vector<uint8_t> pixels = stub_rgb_pixels(path, width, height);
    stbi_write_png(path.c_str(), width, height, 3, pixels.data(), width * 3);
}

} // namespace

std::string dataset_dir(const FixtureSpec& spec) {
    return spec.root_dir + "/dataset";
}

std::string compiled_dir(const FixtureSpec& spec) {
    return spec.root_dir + "/compiled";
}

std::string compiled_bin_path(const FixtureSpec& spec) {
    return compiled_dir(spec) + "/" + spec.split + ".bin";
}

void create_synthetic_dataset(const FixtureSpec& spec) {
    const std::string data_dir = dataset_dir(spec);
    const fs::path split_dir = fs::path(data_dir) / spec.split;

    fs::remove_all(spec.root_dir);
    fs::create_directories(split_dir);

    const int background_images = std::min(spec.num_images, 10);
    const int annotated_images = std::max(spec.num_images - background_images, 0);
    {
        std::ofstream f(data_dir + "/categories.json");
        f << "{\n"
          << "  \"meta\": {\"dataset_name\":\"test\",\"version\":\"1.0\",\"image_format\":\"png\",\n"
          << "           \"image_size_wh\":[" << spec.width << "," << spec.height
          << "],\"bbox_format\":\"xyxy_absolute_pixels\",\n"
          << "           \"mask_format\":\"rle_row_major_start_length\",\n"
          << "           \"background_annotation_policy\":\"empty_jsonl_file\"},\n"
          << "  \"classes\": [\n"
          << "    {\"id\":" << spec.first_class_id << ",\"name\":\"person\"},"
          << "{\"id\":" << spec.first_class_id + 1 << ",\"name\":\"ret\"},"
          << "{\"id\":" << spec.first_class_id + 2 << ",\"name\":\"scope\"},\n"
          << "    {\"id\":" << spec.first_class_id + 3 << ",\"name\":\"iron_sight\"},"
          << "{\"id\":" << spec.first_class_id + 4 << ",\"name\":\"anchor_dot\"},"
          << "{\"id\":" << spec.first_class_id + 5 << ",\"name\":\"glint\"}\n"
          << "  ],\n"
          << "  \"splits\": {\"" << spec.split << "\":{\"total\":" << spec.num_images
          << ",\"background\":" << background_images
          << ",\"annotated\":" << annotated_images << "}}\n"
          << "}";
    }

    const char* class_names[] = {"person", "ret", "scope", "iron_sight", "anchor_dot", "glint"};
    for (int i = 1; i <= spec.num_images; ++i) {
        char fname[64];
        std::snprintf(fname, sizeof(fname), "%06d.png", i);
        write_png_stub((split_dir / fname).string(), spec.width, spec.height);

        std::snprintf(fname, sizeof(fname), "%06d.jsonl", i);
        std::ofstream f(split_dir / fname);
        if (i > 10) {
            const int cls = (i - 11) % 6;
            const int x1 = 10;
            const int y1 = 10;
            const int x2 = std::min(spec.width - 1, 30);
            const int y2 = std::min(spec.height - 1, 30);
            std::string rle;
            for (int row = y1; row < y2; ++row) {
                const int start = row * spec.width + x1;
                const int len = x2 - x1;
                if (!rle.empty()) {
                    rle += " ";
                }
                rle += std::to_string(start) + ":" + std::to_string(len);
            }
            f << R"({"class":")" << class_names[cls]
              << R"(","bbox_xyxy":[)" << x1 << "," << y1 << "," << x2 << "," << y2
              << R"(],"mask_rle_encoding":"row_major_start_length","mask_rle":")"
              << rle << R"(","image_size_wh":[)" << spec.width << "," << spec.height << R"(]})" << "\n";
        }
    }
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

void assert_image_matches(const float* actual, const std::vector<float>& expected) {
    assert(expected.size() > 8);
    for (size_t i = 0; i < expected.size(); ++i) {
        assert(std::fabs(actual[i] - expected[i]) < 1e-6f);
    }
}

} // namespace fastloader::testsupport
