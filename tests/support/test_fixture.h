#pragma once

#include <string>
#include <vector>

namespace mmltk::testsupport {

struct FixtureSpec {
    std::string root_dir;
    std::string split = "train";
    int width = 65;
    int height = 65;
    int num_images = 20;
    int first_class_id = 1;
};

std::string dataset_dir(const FixtureSpec& spec);
std::string compiled_dir(const FixtureSpec& spec);
std::string compiled_bin_path(const FixtureSpec& spec);

void create_synthetic_dataset(const FixtureSpec& spec);
std::vector<float> expected_nchw_stub(const std::string& path, int width, int height);
void assert_image_matches(const float* actual, const std::vector<float>& expected);

} // namespace mmltk::testsupport
