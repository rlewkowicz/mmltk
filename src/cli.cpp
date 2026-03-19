#include "common_utils.h"
#include "compiled_file_utils.h"
#include "compiled_format.h"
#include "dataset_compiler.h"
#include "dataset_loader.h"
#include "profile_utils.h"
#include "rfdetr/progress_bar.h"
#if FASTLOADER_BUILD_RFDETR_NATIVE
#include "rfdetr/cli.h"
#endif
#include <exception>
#include <cstdio>
#include <cstring>
#include <chrono>

using namespace fastloader;

static void print_usage() {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  fastloader compile <source_dir> <output_dir> <split> [width] [height]\n");
    fprintf(stderr, "  fastloader bench <compiled.bin> [batch_size] [num_epochs]\n");
    fprintf(stderr, "  fastloader info <compiled.bin>\n");
#if FASTLOADER_BUILD_RFDETR_NATIVE
    fprintf(stderr, "  fastloader rfdetr <predict|evaluate|train|validate|build-engine|normalize-weights|info> ...\n");
#endif
}

static void cmd_compile(int argc, char** argv) {
    FASTLOADER_PROFILE_RUN_LABEL("fastloader_cli compile");
    if (argc < 5) { print_usage(); return; }
    CompilerConfig cfg;
    cfg.source_dir = argv[2];
    cfg.output_dir = argv[3];
    cfg.split = argv[4];
    if (argc > 5) cfg.target_width = std::stoi(argv[5]);
    if (argc > 6) cfg.target_height = std::stoi(argv[6]);
    else cfg.target_height = cfg.target_width;

    auto t0 = std::chrono::steady_clock::now();
    size_t last_done = 0;
    size_t total = 0;
    fastloader::rfdetr::ProgressBar bar("compile", 0, "img");
    bar.set_postfix("scanning");
    DatasetCompiler::compile(cfg, [&bar, &last_done, &total](size_t done, size_t current_total) {
        if (current_total != total) {
            total = current_total;
            bar.set_total(total);
            if (done == 0 && total > 0) {
                bar.set_postfix("preparing");
            }
        }
        if (done > last_done) {
            const size_t delta = done - last_done;
            last_done = done;
            bar.add(delta);
            bar.set_postfix("");
        }
    });
    bar.close();
    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "compile: done in %.1f seconds\n",
            std::chrono::duration<double>(t1 - t0).count());
}

static void cmd_info(int argc, char** argv) {
    FASTLOADER_PROFILE_RUN_LABEL("fastloader_cli info");
    if (argc < 3) { print_usage(); return; }
    const FileHeader header = read_compiled_header(argv[2]);
    printf("File: %s\n", argv[2]);
    printf("Images: %u\n", header.num_images);
    printf("Size: %ux%u\n", header.image_width, header.image_height);
    printf("Stride: %zu bytes/image (%.2f KB)\n",
           static_cast<size_t>(header.image_stride),
           static_cast<double>(header.image_stride) / 1024.0);
    printf("Classes: %u\n", header.num_classes);
    for (uint32_t i = 0; i < header.num_classes; ++i) {
        printf("  [%u] %s\n", i, header.class_names[i]);
    }
    printf("Batches (bs=1): %u\n", header.num_images);
}

static void cmd_bench(int argc, char** argv) {
    FASTLOADER_PROFILE_RUN_LABEL("fastloader_cli bench");
    if (argc < 3) { print_usage(); return; }
    DatasetLoader::Config cfg;
    cfg.compiled_path = argv[2];
    cfg.batch_size = argc > 3 ? std::stoi(argv[3]) : 32;
    cfg.shuffle = true;
    int num_epochs = argc > 4 ? std::stoi(argv[4]) : 1;

    DatasetLoader loader(cfg);
    printf("Benchmarking: %zu images, batch=%zu, stride=%zu, epochs=%d\n",
           loader.num_images(), cfg.batch_size, loader.image_stride(), num_epochs);

    for (int e = 0; e < num_epochs; ++e) {
        auto t0 = std::chrono::steady_clock::now();
        loader.begin_epoch();
        Batch batch;
        size_t total = 0;
        while (loader.next_batch(batch)) {
            total += batch.num_images;
            loader.release_batch(batch);
        }
        loader.cuda().sync();
        auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        printf("Epoch %d: %zu images in %.2f sec (%.0f img/sec)\n",
               e, total, secs, static_cast<double>(total) / secs);
    }
}

int main(int argc, char** argv) {
    try {
        if (argc < 2) { print_usage(); return 1; }
#if FASTLOADER_BUILD_RFDETR_NATIVE
        if (strcmp(argv[1], "rfdetr") == 0) {
            return fastloader::rfdetr::handle_cli(argc, argv);
        }
#endif
        if (strcmp(argv[1], "compile") == 0) cmd_compile(argc, argv);
        else if (strcmp(argv[1], "info") == 0) cmd_info(argc, argv);
        else if (strcmp(argv[1], "bench") == 0) cmd_bench(argc, argv);
        else { print_usage(); return 1; }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fastloader error: %s\n", error.what());
    } catch (...) {
        std::fputs("fastloader error: unknown exception\n", stderr);
    }
    return 1;
}
