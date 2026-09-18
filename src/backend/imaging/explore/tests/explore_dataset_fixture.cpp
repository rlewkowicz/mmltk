#include "src/backend/imaging/explore/tests/explore_dataset_fixture.h"
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <iomanip>
#include <sstream>
#include "src/backend/data/compiled_format.h"
#include "src/backend/data/dataset_compiler.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/models/rfdetr/augmentation/tests/copy_paste_fixture.h"
namespace mmltk::testsupport {
std::filesystem::path compile_explore_fixture(const std::filesystem::path& temporary_root, const std::string_view fixture_name, const int num_images,
                                              const ExploreFixtureDimensions dimensions, const ExploreFixtureAnnotations annotations) {
    const backend::data::testsupport::FixtureSpec fixture{
        .root_dir = (temporary_root / fixture_name).string(),
        .split = "train",
        .width = dimensions.source_width,
        .height = dimensions.source_height,
        .num_images = num_images,
    };
    backend::data::testsupport::create_synthetic_dataset(fixture);
    if (annotations.objects != 0U) {
        if (annotations.runs_per_object > static_cast<std::size_t>(dimensions.source_height))
            throw std::invalid_argument("Explore fixture mask rows exceed the image height");
        for (int image = 11; image <= num_images; ++image) {
            std::ostringstream filename;
            filename << std::setfill('0') << std::setw(6) << image << ".jsonl";
            std::ofstream output{std::filesystem::path{backend::data::testsupport::dataset_dir(fixture)} / fixture.split / filename.str(), std::ios::trunc};
            for (std::size_t object = 0U; object < annotations.objects; ++object) {
                output << "{\"class\":\"person\",\"bbox_xyxy\":[0,0," << dimensions.source_width << ',' << dimensions.source_height
                       << "],\"mask_rle_encoding\":\"row_major_start_length\",\"mask_rle\":\"";
                for (std::size_t row = 0U; row < annotations.runs_per_object; ++row) {
                    if (row != 0U) output << ' ';
                    output << row * static_cast<std::size_t>(dimensions.source_width) << ":1";
                }
                output << "\",\"image_size_wh\":[" << dimensions.source_width << ',' << dimensions.source_height << "]}\n";
            }
            if (!output) throw std::runtime_error("Explore dense annotation fixture write failed");
        }
    }
    if (annotations.ring_and_dots) {
        if (num_images != 2 || dimensions.source_width != 8 || dimensions.source_height != 8)
            throw std::invalid_argument("ring fixture requires two 8 by 8 images");
        namespace fixture_support = backend::models::rfdetr::test_support;
        for (int image = 1; image <= 2; ++image) {
            const auto name = image == 1 ? "000001.jsonl" : "000002.jsonl";
            std::ofstream output{std::filesystem::path{backend::data::testsupport::dataset_dir(fixture)} / fixture.split / name};
            const auto write = [&](std::span<const backend::data::RLEPair> runs, std::string_view category) {
                output << "{\"class\":\"" << category << "\","
                       << "\"mask_rle_encoding\":\"row_major_start_length\",\"mask_rle\":\"";
                for (std::size_t i = 0; i < runs.size(); ++i) {
                    if (i != 0) output << ' ';
                    output << runs[i].start << ':' << runs[i].length;
                }
                output << "\",\"image_size_wh\":[8,8]}\n";
            };
            if (image == 1)
                for (const auto& run : fixture_support::dot_runs) write(std::span{&run, 1U}, "anchor_dot");
            else
                write(fixture_support::ring_runs, "ret");
            if (!output) throw std::runtime_error("Explore ring fixture write failed");
        }
    }
    mmltk::backend::data::CompilerConfig config;
    config.source_dir = backend::data::testsupport::dataset_dir(fixture);
    config.output_dir = backend::data::testsupport::compiled_dir(fixture);
    config.split = fixture.split;
    config.resize_mode = dimensions.resize_mode;
    config.target_width = dimensions.compiled_width;
    config.target_height = dimensions.compiled_height;
    config.num_workers = 1U;
    const mmltk::backend::data::DatasetCompilePlan plan = mmltk::backend::data::DatasetCompiler::prepare(config, {config.split});
    mmltk::backend::data::DatasetCompiler::compile(plan, 0U);
    return backend::data::testsupport::compiled_bin_path(fixture);
}
std::filesystem::path compile_explore_membership_fixture(const std::filesystem::path& temporary_root) {
    return compile_explore_fixture(temporary_root, "membership-fixture", 12);
}
std::filesystem::path corrupt_explore_label_index(const std::filesystem::path& source, const std::filesystem::path& destination) {
    if (!std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing))
        throw std::runtime_error("failed to copy Explore corruption fixture");
    std::fstream file{destination, std::ios::binary | std::ios::in | std::ios::out};
    if (!file) throw std::runtime_error("failed to open Explore corruption fixture");
    mmltk::backend::data::FileHeader header{};
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    mmltk::backend::data::ImageEntry entry{};
    file.seekg(static_cast<std::streamoff>(header.index_offset));
    file.read(reinterpret_cast<char*>(&entry), sizeof(entry));
    entry.label_offset = sizeof(mmltk::backend::data::PackedInstance);
    file.seekp(static_cast<std::streamoff>(header.index_offset));
    file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
    if (!file) throw std::runtime_error("failed to mutate Explore corruption fixture");
    return destination;
}
}  // namespace mmltk::testsupport
