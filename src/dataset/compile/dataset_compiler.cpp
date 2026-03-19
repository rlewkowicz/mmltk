#include "debug_utils.h"
#include "dataset_compiler.h"
#include "dataset_compiler_internal.h"
#include "profile_utils.h"

#include <cstdio>
#include <filesystem>

namespace fastloader {

void DatasetCompiler::compile(const CompilerConfig& config,
                              const std::function<void(size_t, size_t)>& progress_cb) {
    FASTLOADER_PROFILE_SCOPE("compiler.total");
    const std::filesystem::path split_dir = std::filesystem::path(config.source_dir) / config.split;
    const std::filesystem::path out_dir = config.output_dir;
    std::filesystem::create_directories(out_dir);

    const uint32_t width = config.target_width;
    const uint32_t height = config.target_height;
    const uint32_t channels = 3;
    const size_t image_stride = static_cast<size_t>(channels) * height * width * sizeof(float);

    const int num_workers = compiler_internal::resolve_num_workers(config.num_workers);
    FASTLOADER_PROFILE_SET("compiler.num_workers", static_cast<size_t>(num_workers));

    compiler_internal::DatasetScan scan;
    {
        FASTLOADER_PROFILE_SCOPE("compiler.scan_dataset");
        scan = compiler_internal::scan_dataset(config);
    }
    const auto& class_map = scan.class_map;
    const uint32_t num_images = scan.num_images;
    FASTLOADER_PROFILE_SET("compiler.num_images", num_images);
    FASTLOADER_PROFILE_SET("compiler.image_stride_bytes", image_stride);
    
    // Phase 1: Labels, Phase 2: Pixels, Phase 3: Sync.
    // Total steps = 2 * num_images + 1.
    const size_t total_steps = static_cast<size_t>(num_images) * 2 + 1;
    if (progress_cb) {
        progress_cb(0, total_steps);
    }

    FASTLOADER_DEBUG_LOG("[compile] %s: %u images, %d workers, target %ux%u, stride=%zu bytes/img\n",
                         config.split.c_str(), num_images, num_workers, width, height, image_stride);

    compiler_internal::LabelBlocks label_blocks =
        compiler_internal::build_label_blocks(split_dir,
                                              num_images,
                                              width,
                                              height,
                                              class_map,
                                              num_workers,
                                              [&](size_t done, size_t) {
                                                  if (progress_cb) progress_cb(done, total_steps);
                                              });
    FASTLOADER_DEBUG_LOG("[compile] Labels parsed\n");

    compiler_internal::FileLayout layout;
    {
        FASTLOADER_PROFILE_SCOPE("compiler.compute_layout");
        layout = compiler_internal::compute_layout(num_images,
                                                   image_stride,
                                                   label_blocks.labels.size(),
                                                   label_blocks.rle_pairs.size());
    }

    compiler_internal::assign_pixel_offsets(label_blocks.index, layout.pixel_offset, image_stride);

    FASTLOADER_DEBUG_LOG("[compile] Layout: pixel_offset=%zu (%.1f MB aligned), pixels=%.2f GB, total=%.2f GB\n",
                         layout.pixel_offset,
                         static_cast<double>(layout.pixel_offset) / (1024.0 * 1024.0),
                         static_cast<double>(layout.pixel_blob_size) / (1024.0 * 1024.0 * 1024.0),
                         static_cast<double>(layout.total_size) / (1024.0 * 1024.0 * 1024.0));

    const std::string out_path = (out_dir / (config.split + ".bin")).string();
    FileHandle fd;
    {
        FASTLOADER_PROFILE_SCOPE("compiler.open_output");
        fd = FileHandle::create_output(out_path, layout.total_size);
    }

    const FileHeader header = compiler_internal::make_file_header(num_images,
                                                                  width,
                                                                  height,
                                                                  channels,
                                                                  image_stride,
                                                                  class_map,
                                                                  layout);
    compiler_internal::write_metadata_blocks(fd, layout, header, label_blocks);
    compiler_internal::write_pixel_blob(fd,
                                        split_dir,
                                        num_images,
                                        width,
                                        height,
                                        image_stride,
                                        num_workers,
                                        layout.pixel_offset,
                                        [&](size_t done, size_t) {
                                            if (progress_cb) progress_cb(num_images + done, total_steps);
                                        });
    
    if (progress_cb) progress_cb(2 * num_images + 1, total_steps);
    fd.sync_data();

    FASTLOADER_DEBUG_LOG("[compile] Written %s (%.2f GB)\n", out_path.c_str(),
                         static_cast<double>(layout.total_size) / (1024.0 * 1024.0 * 1024.0));
}

} // namespace fastloader
