#include "src/backend/models/rfdetr/augmentation/sampling.h"
#include "src/backend/models/rfdetr/augmentation/tests/copy_paste_fixture.h"
#include "src/backend/imaging/resample/tests/perceptual_downscale_reference.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <cuda_runtime.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include "src/backend/data/compiled_format.h"
#include "src/backend/models/rfdetr/augmentation/detail/gpu_augment_cuda_launch.h"
#include "src/backend/models/rfdetr/augmentation/detail/gpu_augment_plan_math.h"
#include "src/backend/models/rfdetr/augmentation/gpu_augment.h"
#include "src/backend/models/rfdetr/augmentation/detail/gpu_augmentation_donor_index.h"
#include "src/backend/models/rfdetr/augmentation/spatial_erasure.h"
#include "src/backend/models/rfdetr/augmentation/tests/gpu_augment_test_support.h"
import mmltk.backend.models.rfdetr.augmentation.augmentation_metadata;
namespace mmltk::backend::models::rfdetr {
namespace {
bool has_cuda_device() {
    int count = 0;
    return cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}
void cuda_require(const cudaError_t status) {
    if (status != cudaSuccess) { throw std::runtime_error(cudaGetErrorString(status)); }
}
template <typename T>
class TestDeviceBuffer final {
   public:
    explicit TestDeviceBuffer(const std::size_t count) : count_(count) {
        cuda_require(cudaMalloc(reinterpret_cast<void**>(&data_), count_ * sizeof(T)));
    }  // CLEANUP-IGNORE: The test device buffer and production pinned-host owner have different CUDA custody.
    ~TestDeviceBuffer() {
        if (data_ != nullptr) { (void)cudaFree(data_); }
    }
    TestDeviceBuffer(const TestDeviceBuffer&) = delete;
    TestDeviceBuffer& operator=(const TestDeviceBuffer&) = delete;
    [[nodiscard]] T* data() noexcept { return data_; }
    [[nodiscard]] std::size_t size_bytes() const noexcept { return count_ * sizeof(T); }

   private:
    T* data_ = nullptr;
    std::size_t count_ = 0U;
};
// The same aggregate owns every source/output allocation and the issuing stream.
struct AugmentationPixels final {
    explicit AugmentationPixels(std::size_t count) : input(count), output(count) { cuda_require(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking)); }
    ~AugmentationPixels() {
        (void)cudaStreamSynchronize(stream);
        (void)cudaStreamDestroy(stream);
    }
    TestDeviceBuffer<float> input, output;
    std::unique_ptr<TestDeviceBuffer<float>> donor, boxes;
    std::unique_ptr<TestDeviceBuffer<std::int64_t>> masks;
    cudaStream_t stream = nullptr;
};
void check_model_normalized_rgb(const std::span<const float> model, const std::span<const float> rgb, const std::size_t pixels_per_channel) {
    constexpr std::array means{0.485F, 0.456F, 0.406F};
    constexpr std::array deviations{0.229F, 0.224F, 0.225F};
    REQUIRE(model.size() == rgb.size());
    for (std::size_t value = 0; value < rgb.size(); ++value) {
        const auto channel = (value / pixels_per_channel) % 3U;
        CHECK(std::fabs(model[value] - (rgb[value] - means[channel]) / deviations[channel]) < 1.0e-6F);
    }
}
GpuAugmentationConfig disabled_config() {
    GpuAugmentationConfig config;
    config.enabled = false;
    return config;
}
template <class T>
[[nodiscard]] std::vector<T> repeated_pair(const std::span<const T> values) {
    std::vector<T> result(values.size() * 2U);
    std::ranges::copy(values, result.begin());
    std::ranges::copy(values, result.begin() + static_cast<std::ptrdiff_t>(values.size()));
    return result;
}
std::vector<float> execute_and_copy(GpuAugmentationExecutor& executor, const std::vector<float>& input, const std::span<const std::uint32_t> indices,
                                    const std::span<const std::uint64_t> keys, const std::span<const GpuAugmentationDonor> donors = {},
                                    const std::vector<float>& donor_pixels = {}, const std::vector<std::int64_t>& donor_masks = {},
                                    const std::vector<float>& donor_boxes = {},
                                    const GpuAugmentationDonorSelection donor_selection = GpuAugmentationDonorSelection::Aligned,
                                    const GpuAugmentationOutputDomain output_domain = GpuAugmentationOutputDomain::ModelNormalized, const int extent = 4) {
    auto pixels = std::make_shared<AugmentationPixels>(input.size());
    auto& input_device = pixels->input;
    auto& output_device = pixels->output;
    cuda_require(cudaMemcpy(input_device.data(), input.data(), input_device.size_bytes(), cudaMemcpyHostToDevice));
    auto& donor_device = pixels->donor;
    auto& masks_device = pixels->masks;
    auto& boxes_device = pixels->boxes;
    if (!donor_pixels.empty()) {
        donor_device = std::make_unique<TestDeviceBuffer<float>>(donor_pixels.size());
        cuda_require(cudaMemcpy(donor_device->data(), donor_pixels.data(), donor_device->size_bytes(), cudaMemcpyHostToDevice));
    }
    if (!donor_masks.empty()) {
        masks_device = std::make_unique<TestDeviceBuffer<std::int64_t>>(donor_masks.size());
        cuda_require(cudaMemcpy(masks_device->data(), donor_masks.data(), masks_device->size_bytes(), cudaMemcpyHostToDevice));
    }
    if (!donor_boxes.empty()) {
        boxes_device = std::make_unique<TestDeviceBuffer<float>>(donor_boxes.size());
        cuda_require(cudaMemcpy(boxes_device->data(), donor_boxes.data(), boxes_device->size_bytes(), cudaMemcpyHostToDevice));
    }
    const cudaStream_t stream = pixels->stream;
    const bool indirect = GENERATE(false, true);
    std::vector<const float*> input_slots, donor_slots;
    if (indirect && !indices.empty()) {
        const auto image_values = input.size() / indices.size();
        for (std::size_t image = 0; image < indices.size(); ++image) {
            const auto physical = indices.size() - image - 1U;
            cuda_require(cudaMemcpy(input_device.data() + physical * image_values, input.data() + image * image_values, image_values * sizeof(float),
                                    cudaMemcpyHostToDevice));
            input_slots.push_back(input_device.data() + physical * image_values);
        }
        if (donor_device && !donors.empty()) {
            const auto values = donor_pixels.size() / donors.size();
            for (std::size_t image = 0; image < donors.size(); ++image) {
                const auto physical = donors.size() - image - 1U;
                cuda_require(
                    cudaMemcpy(donor_device->data() + physical * values, donor_pixels.data() + image * values, values * sizeof(float), cudaMemcpyHostToDevice));
                donor_slots.push_back(donor_device->data() + physical * values);
            }
        }
    }
    const GpuAugmentationBatchView batch{
        .input = input_device.data(),
        .output = output_device.data(),
        .image_indices = indices,
        .height = extent,
        .width = extent,
        .output_domain = output_domain,
        .input_slots = input_slots,
        .input_custody = pixels,
        .output_custody = pixels,
        .input_capacity_bytes = indirect ? std::size_t(extent * extent * 3) * sizeof(float) : input_device.size_bytes(),
        .output_capacity_bytes = output_device.size_bytes(),
    };
    const GpuAugmentationDonorBatchView donor_batch{
        .images = donor_device != nullptr ? donor_device->data() : nullptr,
        .masks = masks_device != nullptr ? masks_device->data() : nullptr,
        .boxes = boxes_device != nullptr ? boxes_device->data() : nullptr,
        .mask_words = masks_device != nullptr ? 1 : 0,
        .selection = donor_selection,
        .image_slots = donor_slots,
        .image_custody = pixels,
        .image_capacity_bytes = donor_device ? (indirect ? std::size_t(extent * extent * 3) * sizeof(float) : donor_device->size_bytes()) : 0U,
    };
    (void)executor.Run(batch, keys, donors, donor_batch, stream);
    cuda_require(cudaStreamSynchronize(stream));
    std::vector<float> output(input.size());
    cuda_require(cudaMemcpy(output.data(), output_device.data(), output_device.size_bytes(), cudaMemcpyDeviceToHost));
    executor.Finish();
    return output;
}
// Analytical color fixtures exercise the CUDA effects consumer independently of
// the seeded planner: columns are black, gray, primaries, white and a ramp.
TEST_CASE("augmentation effects preserve RGB and apply the requested output domain", "[backend][models][rfdetr][augmentation][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    constexpr std::array<float, 24U> input{0, .5F, 1, 0, 0, 1, .25F, .75F, 0, .5F, 0, 1, 0, 1, .25F, .75F, 0, .5F, 0, 0, 1, 1, .25F, .75F};
    constexpr std::array<float, 3U> means{.485F, .456F, .406F};
    constexpr std::array<float, 3U> deviations{.229F, .224F, .225F};
    TestDeviceBuffer<float> source(input.size());
    TestDeviceBuffer<float> output(input.size());
    TestDeviceBuffer<float> parameters(kGpuAugmentationParameterCount);
    TestDeviceBuffer<std::uint64_t> keys(1U);
    constexpr std::uint64_t key = 17U;
    cuda_require(cudaMemcpy(source.data(), input.data(), source.size_bytes(), cudaMemcpyHostToDevice));
    cuda_require(cudaMemcpy(keys.data(), &key, sizeof(key), cudaMemcpyHostToDevice));
    for (const int effect : {0, 1, 2}) {
        std::array<float, kGpuAugmentationParameterCount> values{};
        values[augment_math::kInverse00] = values[augment_math::kInverse11] = 1.0F;
        const auto matrix = augment_math::kColorMatrix;
        values[matrix] = values[matrix + 4] = values[matrix + 8] = 1.0F;
        if (effect == 1) {
            values[matrix] = 2.0F;
            values[matrix + 4] = .5F;
            values[augment_math::kColorOffset + 2] = -.25F;
        } else if (effect == 2) {
            // Cyclic channel permutation: red <- blue <- green <- red.
            values[matrix] = values[matrix + 4] = values[matrix + 8] = 0.0F;
            values[matrix + 2] = values[matrix + 3] = values[matrix + 7] = 1.0F;
        }
        cuda_require(cudaMemcpy(parameters.data(), values.data(), parameters.size_bytes(), cudaMemcpyHostToDevice));
        for (const bool remap : {false, true})
            for (const bool explicit_keys : {false, true})
                for (const auto domain : {GpuAugmentationOutputDomain::UnitRgb, GpuAugmentationOutputDomain::ModelNormalized}) {
                    CAPTURE(effect, remap, explicit_keys, domain);
                    if (explicit_keys)
                        launch_gpu_augmentation_images_explicit(source.data(), output.data(), parameters.data(), nullptr, nullptr, nullptr, nullptr, 0,
                                                                keys.data(), 1, 1, 8, {}, remap, domain, nullptr);
                    else
                        launch_gpu_augmentation_images(source.data(), output.data(), parameters.data(), nullptr, nullptr, nullptr, nullptr, 0, 1, 1, 8, {}, 17U,
                                                       0, 0, 0U, remap, domain, nullptr);
                    std::array<float, input.size()> actual{};
                    cuda_require(cudaMemcpy(actual.data(), output.data(), output.size_bytes(), cudaMemcpyDeviceToHost));
                    for (std::size_t channel = 0; channel < 3U; ++channel)
                        for (std::size_t pixel = 0; pixel < 8U; ++pixel) {
                            float expected = input[channel * 8U + pixel];
                            if (effect == 1) {
                                if (channel == 0) expected = std::min(1.0F, expected * 2.0F);
                                if (channel == 1) expected *= .5F;
                                if (channel == 2) expected = std::max(0.0F, expected - .25F);
                            } else if (effect == 2) {
                                expected = input[((channel + 2U) % 3U) * 8U + pixel];
                            }
                            if (domain == GpuAugmentationOutputDomain::ModelNormalized) expected = (expected - means[channel]) / deviations[channel];
                            CHECK(std::fabs(actual[channel * 8U + pixel] - expected) < 1.0e-6F);
                        }
                }
    }
}
TEST_CASE("augmentation executor selects display RGB for both input formats and identity routes", "[backend][models][rfdetr][augmentation][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    constexpr std::array<std::uint32_t, 1U> indices{3U};
    constexpr std::array<std::uint64_t, 1U> keys{17U};
    constexpr std::array<std::uint8_t, 4U> samples{0U, 64U, 128U, 255U};
    std::array<std::uint8_t, 64U> rgba{};
    std::array<float, 48U> planar{};
    for (std::size_t pixel = 0; pixel < 16U; ++pixel) {
        for (std::size_t channel = 0; channel < 3U; ++channel) {
            const auto value = samples[(pixel + channel) % samples.size()];
            rgba[pixel * 4U + channel] = value;
            planar[channel * 16U + pixel] = static_cast<float>(value) / 255.0F;
        }
        rgba[pixel * 4U + 3U] = 7U;  // Alpha is not a color channel.
    }
    TestDeviceBuffer<float> input(planar.size());
    TestDeviceBuffer<std::uint8_t> rgba_input(rgba.size());
    TestDeviceBuffer<float> output(planar.size());
    cuda_require(cudaMemcpy(input.data(), planar.data(), input.size_bytes(), cudaMemcpyHostToDevice));
    cuda_require(cudaMemcpy(rgba_input.data(), rgba.data(), rgba_input.size_bytes(), cudaMemcpyHostToDevice));
    for (const auto& config : {disabled_config(), test_support::isolated_augmentation_config(), test_support::isolated_augmentation_config(1.0F)}) {
        test_support::AugmentationExecution execution_executor(0);
        GpuAugmentationExecutor executor(config, 1U, 4, 4, execution_executor.context, execution_executor.retirement);
        for (const auto format : {GpuAugmentationInputFormat::PlanarFloat32, GpuAugmentationInputFormat::Rgba8}) {
            const GpuAugmentationBatchView batch{
                .input = format == GpuAugmentationInputFormat::Rgba8 ? static_cast<const void*>(rgba_input.data()) : static_cast<const void*>(input.data()),
                .output = output.data(),
                .image_indices = indices,
                .height = 4,
                .width = 4,
                .input_format = format,
                .output_domain = GpuAugmentationOutputDomain::UnitRgb};
            const std::array<GpuAugmentationDonor, 1U> missing_donor{};
            (void)executor.Run(batch, keys, missing_donor, {}, nullptr);
            std::array<float, planar.size()> actual{};
            cuda_require(cudaMemcpy(actual.data(), output.data(), output.size_bytes(), cudaMemcpyDeviceToHost));
            CHECK(executor.plan().images.front().paste_donor_slot < 0);
            for (std::size_t value = 0; value < actual.size(); ++value) CHECK(std::fabs(actual[value] - planar[value]) < 1.0e-6F);
        }
    }
}
TEST_CASE("native augmentation resolves exact visible support", "[backend][models][rfdetr][augmentation][support]") {
    using mmltk::backend::data::PackedInstance;
    using mmltk::backend::data::RLEPair;
    const std::array runs{RLEPair{9, 3}, RLEPair{17, 3}, RLEPair{25, 3}, RLEPair{10, 2}, RLEPair{18, 2}, RLEPair{26, 2}};
    PackedInstance source{.class_id = 2,
                          .flags = mmltk::backend::data::kAnnotationMask,
                          .bbox_x1 = 0,
                          .bbox_y1 = 0,
                          .bbox_x2 = 5,
                          .bbox_y2 = 5,
                          .mask_rle_offset = 0,
                          .mask_rle_pairs = 3};
    PackedInstance donor{.class_id = 4,
                         .flags = mmltk::backend::data::kAnnotationMask,
                         .bbox_x1 = 0,
                         .bbox_y1 = 0,
                         .bbox_x2 = 5,
                         .bbox_y2 = 5,
                         .mask_rle_offset = 3 * sizeof(RLEPair),
                         .mask_rle_pairs = 3};
    AugmentationImagePlan plan;
    plan.paste_donor_slot = 0;
    plan.paste_source_box = {0, 0, 0.625F, 0.625F};
    plan.paste_output_box = plan.paste_source_box;
    plan.paste_masked = true;
    plan.paste_support = runs.data() + 3;
    plan.paste_support_count = 3;
    std::vector<AugmentationPreviewAnnotation> output;
    SECTION("partial edge occlusion trims support and preserves source identity") {
        build_augmentation_preview_annotations(std::span{&source, 1U}, &donor, &plan, 8, 8, output, runs);
        REQUIRE(output.size() == 2);
        CHECK(output[0].source_ordinal == 0);
        CHECK(output[0].box_xyxy == std::array<float, 4>{0.125F, 0.125F, 0.25F, 0.5F});
        CHECK(output[0].visible_area_pixels == 3.0F);
        CHECK(output[0].occluder_index == 1);
        CHECK(output[1].visible_area_pixels == 6.0F);
    }
    SECTION("full mask occlusion removes source despite uncovered box margins") {
        plan.paste_support = runs.data();
        build_augmentation_preview_annotations(std::span{&source, 1U}, &source, &plan, 8, 8, output, runs);
        REQUIRE(output.size() == 1);
        CHECK(output[0].source_ordinal == 1);
        CHECK(output[0].occluder_index == -1);
    }
    SECTION("interior hole reduces area without shrinking enclosing box") {
        const RLEPair hole{18, 1};
        plan.paste_support = &hole;
        plan.paste_support_count = 1;
        build_augmentation_preview_annotations(std::span{&source, 1U}, &donor, &plan, 8, 8, output, runs);
        REQUIRE(output.size() == 2);
        CHECK(output[0].box_xyxy == std::array<float, 4>{0.125F, 0.125F, 0.5F, 0.5F});
        CHECK(output[0].visible_area_pixels == 8.0F);
    }
    SECTION("compaction retains original ordinals and repairs donor indexes") {
        const std::array sources{donor, source};
        build_augmentation_preview_annotations(sources, &donor, &plan, 8, 8, output, runs);
        REQUIRE(output.size() == 2);
        CHECK(output[0].source_ordinal == 1);
        CHECK(output[0].occluder_index == 1);
        CHECK(output[1].source_ordinal == 2);
    }
    SECTION("preview metadata preserves dataset mask offsets beyond four GiB") {
        const std::uint64_t source_offset = std::uint64_t{1} << 32U;
        source.mask_rle_offset = source_offset;
        source.mask_rle_pairs = 0U;
        source.flags &= ~mmltk::backend::data::kAnnotationMask;
        donor.mask_rle_offset = source_offset + sizeof(RLEPair);
        build_augmentation_preview_annotations(std::span{&source, 1U}, &donor, &plan, 8, 8, output, runs);
        REQUIRE(output.size() == 2);
        CHECK(output[0].mask_rle_offset == source_offset);
        CHECK(output[1].mask_rle_offset == source_offset + sizeof(RLEPair));
    }
    SECTION("malformed masks fail before visibility culling") {
        const std::array malformed{RLEPair{63, 2}};
        source.mask_rle_pairs = 1;
        plan.erasure.dropout_probability = 1;
        CHECK_THROWS_AS(build_augmentation_preview_annotations(std::span{&source, 1U}, &donor, &plan, 8, 8, output, malformed), std::runtime_error);
    }
    SECTION("erasure removes both source and donor") {
        plan.erasure.dropout_probability = 1;
        build_augmentation_preview_annotations(std::span{&source, 1U}, &donor, &plan, 8, 8, output, runs);
        CHECK(output.empty());
    }
    SECTION("box-only source uses actual irregular donor footprint") {
        source.mask_rle_pairs = 0;
        source.flags &= ~mmltk::backend::data::kAnnotationMask;
        build_augmentation_preview_annotations(std::span{&source, 1U}, &donor, &plan, 8, 8, output, runs);
        REQUIRE(output.size() == 2);
        CHECK(output[0].visible_area_pixels == 19.0F);
    }
    SECTION("rectangular image paste uses rectangle despite donor segmentation") {
        plan.paste_masked = false;
        build_augmentation_preview_annotations(std::span{&source, 1U}, &donor, &plan, 8, 8, output, runs);
        REQUIRE(output.size() == 1);
        CHECK(output[0].source_ordinal == 1);
        CHECK(output[0].box_xyxy == plan.paste_output_box);
        CHECK(output[0].visible_area_pixels == 25.0F);
    }
    SECTION("known empty masked donor pastes no support and leaves source geometry unchanged") {
        plan.paste_support = nullptr;
        plan.paste_support_count = 0;
        donor.mask_rle_pairs = 0;
        build_augmentation_preview_annotations(std::span{&source, 1U}, &donor, &plan, 8, 8, output, runs);
        REQUIRE(output.size() == 1);
        CHECK(output[0].box_xyxy == std::array<float, 4>{0, 0, .625F, .625F});
        CHECK(output[0].visible_area_pixels == 9.F);
        CHECK(output[0].occluder_index == -1);
    }
    SECTION("identity and photometric plans resolve masks and preserve empty records") {
        const auto original_source = source;
        for (const bool masked : {true, false}) {
            CAPTURE(masked);
            source = original_source;
            if (masked) {
                source.bbox_x2 = 1;
                source.bbox_y2 = 1;
            } else {
                source.bbox_x2 = source.bbox_x1;
                source.mask_rle_pairs = 0;
                source.flags &= ~mmltk::backend::data::kAnnotationMask;
            }
            plan = {};
            for (const auto* identity : {static_cast<const AugmentationImagePlan*>(nullptr), static_cast<const AugmentationImagePlan*>(&plan)}) {
                build_augmentation_preview_annotations(std::span{&source, 1U}, nullptr, identity, 8, 8, output, runs);
                REQUIRE(output.size() == 1);
                if (masked) {
                    CHECK(output[0].box_xyxy == std::array<float, 4>{0, 0, 0.125F, 0.125F});
                    CHECK(output[0].visible_area_pixels == 9.0F);
                } else {
                    CHECK(output[0].box_xyxy[0] == output[0].box_xyxy[2]);
                }
            }
        }
    }
    SECTION("zero-length mask runs are malformed") {
        const std::array empty_run{RLEPair{9, 0}};
        source.mask_rle_pairs = 1;
        CHECK_THROWS_AS(build_augmentation_preview_annotations(std::span{&source, 1U}, nullptr, nullptr, 8, 8, output, empty_run), std::runtime_error);
    }
    SECTION("translation preserves continuous box clipping independently of mask edges") {
        plan.paste_donor_slot = -1;
        plan.forward[2] = -0.25F;
        plan.inverse[2] = 0.25F;
        build_augmentation_preview_annotations(std::span{&source, 1U}, nullptr, &plan, 8, 8, output, runs);
        REQUIRE(output.size() == 1);
        CHECK(output[0].box_xyxy == std::array<float, 4>{0, 0, 0.375F, 0.625F});
        CHECK(output[0].visible_area_pixels == 6.0F);
    }
}
TEST_CASE("augmentation preview donor selection is deterministic and excludes its source", "[backend][models][rfdetr][augmentation]") {
    constexpr std::array<std::uint32_t, 3U> annotated{4U, 9U, 15U};
    const std::uint64_t key = augmentation_preview_image_key(17U, 7U, 9U);
    CHECK(augmentation_preview_image_key(17U, 7U, 9U) == key);
    CHECK(augmentation_preview_image_key(18U, 7U, 9U) != key);
    CHECK(augmentation_preview_image_key(17U, 8U, 9U) != key);
    const std::uint32_t donor = select_augmentation_preview_donor_image(annotated, 9U, key);
    CHECK(donor != 9U);
    CHECK(select_augmentation_preview_donor_image(annotated, 9U, key) == donor);
    CHECK(select_augmentation_preview_donor_image({}, 9U, key) == 9U);
    CHECK(select_augmentation_preview_donor_instance(0U, key) == 0U);
    CHECK(select_augmentation_preview_donor_instance(5U, key) < 5U);
    constexpr std::array<std::uint32_t, 5U> full_catalog{1U, 4U, 9U, 15U, 27U};
    const auto full_donor = select_augmentation_preview_donor_image(full_catalog, 9U, key);
    CHECK(full_donor != 9U);
    CHECK(std::ranges::find(full_catalog, full_donor) != full_catalog.end());
    CHECK(select_augmentation_preview_donor_image(full_catalog, 9U, key) == full_donor);
    constexpr std::array<std::uint32_t, 2U> replacement_catalog{31U, 42U};
    const auto replacement_key = augmentation_preview_image_key(18U, 7U, 9U);
    const auto replacement_donor = select_augmentation_preview_donor_image(replacement_catalog, 9U, replacement_key);
    CHECK(std::ranges::find(replacement_catalog, replacement_donor) != replacement_catalog.end());
    CHECK(std::ranges::find(full_catalog, replacement_donor) == full_catalog.end());
}
TEST_CASE("raw augmentation rejects invalid configuration before allocating", "[backend][models][rfdetr][augmentation]") {
    GpuAugmentationConfig invalid;
    invalid.color.probability = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(gpu_augmentation_config_valid(invalid));
    if (!has_cuda_device()) SKIP("CUDA context unavailable for executor construction");
    test_support::AugmentationExecution execution;
    CHECK_THROWS(GpuAugmentationExecutor(invalid, 1U, 4, 4, execution.context, execution.retirement));
}
TEST_CASE("raw augmentation rejects overflowing two-slot parameter staging before CUDA access", "[backend][models][rfdetr][augmentation]") {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    constexpr auto parameters = static_cast<std::size_t>(kGpuAugmentationParameterCount);
    constexpr std::size_t staging_slots = 2U;
    constexpr auto capacity = maximum / (staging_slots * parameters) + 1U;
    STATIC_REQUIRE(capacity <= maximum / parameters);
    STATIC_REQUIRE(capacity <= maximum / 3U);
    STATIC_REQUIRE(capacity <= maximum / (staging_slots * static_cast<std::size_t>(kGpuCopyPasteParameterCount)));
    if (!has_cuda_device()) SKIP("CUDA context unavailable for executor construction");
    test_support::AugmentationExecution execution;
    CHECK_THROWS_WITH(GpuAugmentationExecutor(disabled_config(), capacity, 1, 1, execution.context, execution.retirement),
                      "GPU augmentation workspace size overflows");
}
TEST_CASE("raw augmentation is deterministic, seed-sensitive, bounded, and reuses high-water workspace", "[backend][models][rfdetr][augmentation][cuda]") {
    if (!has_cuda_device()) { SKIP("CUDA device unavailable"); }
    cuda_require(cudaSetDevice(0));
    constexpr std::array<std::uint32_t, 1U> indices{3U};
    constexpr std::array<std::uint64_t, 1U> key_a{17U};
    constexpr std::array<std::uint64_t, 1U> key_b{29U};
    std::vector<float> input(3U * 4U * 4U);
    for (std::size_t index = 0U; index < input.size(); ++index) { input[index] = static_cast<float>(index) / static_cast<float>(input.size()); }
    test_support::AugmentationExecution execution_identity(0);
    GpuAugmentationExecutor identity(disabled_config(), 2U, 4, 4, execution_identity.context, execution_identity.retirement);
    const auto identity_output = execute_and_copy(identity, input, indices, key_a);
    CHECK(identity.plan().active_size == 1U);
    CHECK_FALSE(identity.plan().transforms_geometry);
    CHECK(identity.plan().images.front().forward == AugmentationImagePlan{}.forward);
    REQUIRE(identity_output.size() == input.size());
    check_model_normalized_rgb(identity_output, input, 16U);
    constexpr std::array<std::uint32_t, 2U> full_indices{3U, 4U};
    constexpr std::array<std::uint64_t, 2U> full_keys{17U, 23U};
    auto full_input = repeated_pair<float>(input);
    (void)execute_and_copy(identity, full_input, full_indices, full_keys);
    const std::size_t at_capacity_bytes = identity.workspace_capacity_bytes();
    CHECK(at_capacity_bytes == identity.device_capacity_bytes() + identity.pinned_capacity_bytes());
    const auto pinned_capacity = identity.pinned_capacity_bytes();
    const auto device_capacity = identity.device_capacity_bytes();
    (void)execute_and_copy(identity, input, indices, key_a);
    CHECK(identity.plan().active_size == 1U);
    CHECK(identity.workspace_capacity_bytes() == at_capacity_bytes);
    CHECK(identity.pinned_capacity_bytes() == pinned_capacity);
    CHECK(identity.device_capacity_bytes() == device_capacity);
    std::vector<std::uint8_t> rgba(4U * 4U * 4U);
    std::vector<float> planar(3U * 4U * 4U);
    for (std::size_t pixel = 0U; pixel < 16U; ++pixel) {
        rgba[pixel * 4U] = static_cast<std::uint8_t>(pixel * 7U);
        rgba[pixel * 4U + 1U] = static_cast<std::uint8_t>(pixel * 5U);
        rgba[pixel * 4U + 2U] = static_cast<std::uint8_t>(pixel * 3U);
        rgba[pixel * 4U + 3U] = 255U;
        planar[pixel] = static_cast<float>(rgba[pixel * 4U]) / 255.0F;
        planar[16U + pixel] = static_cast<float>(rgba[pixel * 4U + 1U]) / 255.0F;
        planar[32U + pixel] = static_cast<float>(rgba[pixel * 4U + 2U]) / 255.0F;
    }
    const auto expected_rgba = execute_and_copy(identity, planar, indices, key_a);
    auto full_rgba = repeated_pair<std::uint8_t>(rgba);
    TestDeviceBuffer<std::uint8_t> rgba_device(full_rgba.size());
    TestDeviceBuffer<float> rgba_output(2U * planar.size());
    cuda_require(cudaMemcpy(rgba_device.data(), full_rgba.data(), rgba_device.size_bytes(), cudaMemcpyHostToDevice));
    cudaStream_t rgba_stream = nullptr;
    cuda_require(cudaStreamCreateWithFlags(&rgba_stream, cudaStreamNonBlocking));
    const GpuAugmentationBatchView full_rgba_batch{
        .input = rgba_device.data(),
        .output = rgba_output.data(),
        .image_indices = full_indices,
        .height = 4,
        .width = 4,
        .input_format = GpuAugmentationInputFormat::Rgba8,
    };
    (void)identity.Run(full_rgba_batch, full_keys, {}, {}, rgba_stream, 1U);
    cuda_require(cudaStreamSynchronize(rgba_stream));
    const std::size_t rgba_capacity = identity.workspace_capacity_bytes();
    const GpuAugmentationBatchView smaller_rgba_batch{
        .input = rgba_device.data(),
        .output = rgba_output.data(),
        .image_indices = indices,
        .height = 4,
        .width = 4,
        .input_format = GpuAugmentationInputFormat::Rgba8,
    };
    (void)identity.Run(smaller_rgba_batch, key_a, {}, {}, rgba_stream, 0U);
    cuda_require(cudaStreamSynchronize(rgba_stream));
    CHECK(identity.workspace_capacity_bytes() == rgba_capacity);
    std::vector<float> observed_rgba(planar.size());
    cuda_require(cudaMemcpy(observed_rgba.data(), rgba_output.data(), observed_rgba.size() * sizeof(float), cudaMemcpyDeviceToHost));
    cuda_require(cudaStreamDestroy(rgba_stream));
    REQUIRE(observed_rgba.size() == expected_rgba.size());
    for (std::size_t value = 0U; value < observed_rgba.size(); ++value) { CHECK(std::fabs(observed_rgba[value] - expected_rgba[value]) < 1.0e-6F); }
    GpuAugmentationConfig color = test_support::isolated_augmentation_config();
    color.color = {1.0F, 1.0F, 1.0F};
    test_support::AugmentationExecution execution_executor(0);
    GpuAugmentationExecutor executor(color, 2U, 4, 4, execution_executor.context, execution_executor.retirement);
    const std::size_t capacity_bytes = executor.workspace_capacity_bytes();
    const auto first = execute_and_copy(executor, input, indices, key_a);
    executor.Reconfigure(color);
    const auto repeated = execute_and_copy(executor, input, indices, key_a);
    const auto changed = execute_and_copy(executor, input, indices, key_b);
    CHECK(first == repeated);
    CHECK(first != changed);
    CHECK(executor.workspace_capacity_bytes() == capacity_bytes);
    color.geometry = {1.0F, 1.0F, 1.0F};
    color.resize = {1.0F, 1.0F, 1.0F};
    executor.Reconfigure(color);
    std::vector<float> transformed_pixels;
    AugmentationImagePlan transformed_plan;
    bool observed_crop = false;
    for (std::uint64_t key = 0U; key < 64U && !observed_crop; ++key) {
        const std::array current_key{key};
        transformed_pixels = execute_and_copy(executor, input, indices, current_key);
        observed_crop = executor.plan().images.front().resize_scale > 1.0F;
        if (observed_crop) { transformed_plan = executor.plan().images.front(); }
    }
    CHECK(executor.plan().transforms_geometry);
    REQUIRE(observed_crop);
    CHECK(transformed_plan.resize_scale > 1.0F);
    CHECK(transformed_pixels != identity_output);
    const mmltk::backend::data::PackedInstance source_box{
        .class_id = 0U,
        .flags = 0U,
        .bbox_x1 = 1,
        .bbox_y1 = 1,
        .bbox_x2 = 3,
        .bbox_y2 = 3,
        .mask_rle_offset = 0U,
        .mask_rle_pairs = 0U,
    };
    const AugmentationMappedInstance mapped = map_augmentation_instance(source_box, 4, 4, &transformed_plan);
    CHECK(mapped.visible);
    CHECK(mapped.output_box_xyxy == transform_augmentation_box_xyxy(mapped.source_box_xyxy, transformed_plan.forward));
    CHECK(mapped.output_box_xyxy != mapped.source_box_xyxy);
    CHECK(executor.workspace_capacity_bytes() == capacity_bytes);
    bool observed_flip = false;
    for (std::uint64_t key = 0U; key < 64U && !observed_flip; ++key) {
        const std::array current_key{key};
        (void)execute_and_copy(executor, input, indices, current_key);
        const auto& transform = executor.plan().images.front().forward;
        observed_flip = transform[0] * transform[4] - transform[1] * transform[3] < 0.0F;
    }
    CHECK(observed_flip);
    GpuAugmentationConfig invalid = color;
    invalid.blur.min_strength = 0.9F;
    invalid.blur.max_strength = 0.1F;
    CHECK_THROWS(executor.Reconfigure(invalid));
    TestDeviceBuffer<float> device(input.size());
    cudaStream_t stream = nullptr;
    cuda_require(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    const GpuAugmentationBatchView empty{.input = device.data(), .output = device.data(), .image_indices = {}, .height = 4, .width = 4};
    (void)executor.Run(empty, {}, {}, {}, stream);
    CHECK(executor.plan().active_size == 0U);
    constexpr std::array<std::uint32_t, 3U> too_many_indices{0U, 1U, 2U};
    constexpr std::array<std::uint64_t, 3U> too_many_keys{1U, 2U, 3U};
    const GpuAugmentationBatchView too_many{
        .input = device.data(),
        .output = device.data(),
        .image_indices = too_many_indices,
        .height = 4,
        .width = 4,
    };
    CHECK_THROWS((void)executor.Run(too_many, too_many_keys, {}, {}, stream));
    cuda_require(cudaStreamDestroy(stream));
}
TEST_CASE("raw augmentation handles missing and valid copy-paste donors with masks", "[backend][models][rfdetr][augmentation][cuda]") {
    if (!has_cuda_device()) { SKIP("CUDA device unavailable"); }
    cuda_require(cudaSetDevice(0));
    GpuAugmentationConfig config = test_support::isolated_augmentation_config();
    config.copy_paste_probability = 1.0F;
    test_support::AugmentationExecution execution_executor(0);
    GpuAugmentationExecutor executor(config, 1U, 4, 4, execution_executor.context, execution_executor.retirement);
    constexpr std::array<std::uint32_t, 1U> indices{4U};
    constexpr std::array<std::uint64_t, 1U> keys{83U};
    std::vector<float> source(3U * 4U * 4U, 0.1F);
    std::vector<float> donor_pixels(source.size(), 0.9F);
    std::vector<std::int64_t> masks(1U, -1);
    std::vector<float> boxes{0.0F, 0.0F, 1.0F, 1.0F};
    const std::array<GpuAugmentationDonor, 1U> missing{{
        {.label = -1, .dataset_index = 8U, .area = 16.0F, .box = {0.0F, 0.0F, 1.0F, 1.0F}, .has_mask = true},
    }};
    const auto without_donor = execute_and_copy(executor, source, indices, keys, missing);
    CHECK(executor.plan().images.front().paste_donor_slot < 0);
    test_support::AugmentationExecution execution_no_copy_paste(0);
    GpuAugmentationExecutor no_copy_paste(test_support::isolated_augmentation_config(), 1U, 4, 4, execution_no_copy_paste.context,
                                          execution_no_copy_paste.retirement);
    const auto copy_paste_disabled = execute_and_copy(no_copy_paste, source, indices, keys);
    REQUIRE(without_donor.size() == copy_paste_disabled.size());
    for (std::size_t value = 0U; value < without_donor.size(); ++value) { CHECK(std::fabs(without_donor[value] - copy_paste_disabled[value]) < 1.0e-6F); }
    const std::array<GpuAugmentationDonor, 1U> donor{{
        {.label = 2, .dataset_index = 8U, .area = 16.0F, .box = {0.0F, 0.0F, 1.0F, 1.0F}, .has_mask = true},
    }};
    const auto pasted = execute_and_copy(executor, source, indices, keys, donor, donor_pixels, masks, boxes);
    REQUIRE(executor.plan().images.front().paste_donor_slot == 0);
    CHECK(executor.plan().images.front().paste_label == 2);
    CHECK(std::ranges::any_of(pasted, [](const float value) { return value > 1.0F; }));
    const auto display = execute_and_copy(executor, source, indices, keys, donor, donor_pixels, masks, boxes, GpuAugmentationDonorSelection::Aligned,
                                          GpuAugmentationOutputDomain::UnitRgb);
    CHECK(std::ranges::any_of(display, [](const float value) { return value > .8F; }));
    CHECK(std::ranges::all_of(display, [](const float value) { return value >= 0.0F && value <= 1.0F; }));
    check_model_normalized_rgb(pasted, display, 16U);
}
TEST_CASE("copy-paste physical ring support is independent of loss selection", "[backend][augmentation][copy_paste][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA device unavailable");
    cuda_require(cudaSetDevice(0));
    constexpr std::array<std::uint32_t, 1> indices{0};
    constexpr std::array<std::uint64_t, 1> keys{83};
    std::vector<float> source(192, .1F), pixels(192, .9F), boxes{.125F, .125F, .875F, .875F};
    std::uint64_t bits = 0;
    for (int p = 0; p < 64; ++p)
        if (test_support::fixture_contains(test_support::ring_runs, p % 8, p / 8)) bits |= 1ULL << p;
    const std::vector<std::int64_t> masks{static_cast<std::int64_t>(bits)};
    for (const bool masked : {false, true})
        for (const bool present : {false, true})
            for (const float probability : {0.F, 1.F}) {
                CAPTURE(masked, present, probability);
                test_support::AugmentationExecution execution_executor(0);
                GpuAugmentationExecutor executor(test_support::isolated_augmentation_config(probability), 1, 8, 8, execution_executor.context,
                                                 execution_executor.retirement);
                const std::array donors{GpuAugmentationDonor{
                    .label = present ? 7 : -1, .dataset_index = 1, .area = masked ? 20.F : 36.F, .box = {.125F, .125F, .875F, .875F}, .has_mask = masked}};
                const auto display = execute_and_copy(executor, source, indices, keys, donors, pixels, masks, boxes, GpuAugmentationDonorSelection::Aligned,
                                                      GpuAugmentationOutputDomain::UnitRgb, 8);
                const auto plan = executor.plan().images[0];
                CHECK((plan.paste_donor_slot >= 0) == (present && probability == 1));
                if (plan.paste_donor_slot >= 0) CHECK(plan.paste_masked == masked);
                for (int y = 0; y < 8; ++y)
                    for (int x = 0; x < 8; ++x) {
                        const bool support = test_support::ring_paste_contains(plan.paste_inverse, masked, x, y);
                        const float expected = plan.paste_donor_slot >= 0 && support ? .9F : .1F;
                        for (int c = 0; c < 3; ++c) CHECK(std::abs(display[c * 64 + y * 8 + x] - expected) < 1.e-6F);
                    }
                const auto normalized = execute_and_copy(executor, source, indices, keys, donors, pixels, masks, boxes, GpuAugmentationDonorSelection::Aligned,
                                                         GpuAugmentationOutputDomain::ModelNormalized, 8);
                check_model_normalized_rgb(normalized, display, 64);
            }
}
TEST_CASE("raw augmentation executor settles submitted work during shutdown", "[backend][models][rfdetr][augmentation][cuda]") {
    if (!has_cuda_device()) { SKIP("CUDA device unavailable"); }
    cuda_require(cudaSetDevice(0));
    constexpr std::array<std::uint32_t, 1U> indices{7U};
    constexpr std::array<std::uint64_t, 1U> keys{31U};
    std::vector<float> input(3U * 4U * 4U, 0.25F);
    auto pixels = std::make_shared<AugmentationPixels>(input.size());
    auto& input_device = pixels->input;
    auto& output_device = pixels->output;
    cuda_require(cudaMemcpy(input_device.data(), input.data(), input_device.size_bytes(), cudaMemcpyHostToDevice));
    cudaStream_t stream = nullptr;
    cuda_require(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
    {
        test_support::AugmentationExecution execution_executor(0);
        GpuAugmentationExecutor executor(disabled_config(), 1U, 4, 4, execution_executor.context, execution_executor.retirement);
        const GpuAugmentationBatchView batch{
            .input = input_device.data(),
            .output = output_device.data(),
            .image_indices = indices,
            .height = 4,
            .width = 4,
        };
        (void)executor.Run(batch, keys, {}, {}, stream);
    }
    std::vector<float> output(input.size());
    cuda_require(cudaMemcpy(output.data(), output_device.data(), output_device.size_bytes(), cudaMemcpyDeviceToHost));
    CHECK(std::fabs(output.front() - (0.25F - 0.485F) / 0.229F) < 1.0e-6F);
    cuda_require(cudaStreamDestroy(stream));
}
TEST_CASE("spatial erasure uses output pixel centers and half-open rectangle edges", "[backend][models][rfdetr][augmentation]") {
    const AugmentationSpatialErasure rectangle{.x0 = 0.125F, .y0 = 0.125F, .x1 = 0.625F, .y1 = 0.625F, .rectangular = 1U};
    CHECK(augment_math::erases_pixel(rectangle, 0, 0, 4, 4));
    CHECK(augment_math::erases_pixel(rectangle, 1, 1, 4, 4));
    CHECK_FALSE(augment_math::erases_pixel(rectangle, 2, 1, 4, 4));
    CHECK_FALSE(augment_math::erases_pixel(rectangle, 1, 2, 4, 4));
    CHECK(augment_math::erases_sample(rectangle, 0.0F, 0.0F, 4, 4));
    CHECK_FALSE(augment_math::erases_sample(rectangle, 1.0F, 1.0F, 4, 4));
    CHECK_FALSE(augment_math::erases_pixel({}, 0, 0, 4, 4));
    CHECK(augment_math::erases_pixel({.dropout_probability = 1.0F}, 3, 3, 4, 4));
}
TEST_CASE("planned spatial erasure agrees with final image pixels through geometry and donor composition", "[backend][models][rfdetr][augmentation]") {
    if (!has_cuda_device()) { SKIP("CUDA unavailable"); }
    constexpr std::size_t count = 96U;
    std::array<std::uint32_t, count> indices{};
    std::array<std::uint64_t, count> keys{};
    std::array<GpuAugmentationDonor, count> donors{};
    for (std::size_t image = 0U; image < count; ++image) {
        indices[image] = static_cast<std::uint32_t>(image);
        keys[image] = image;
        donors[image] = {.label = 1, .dataset_index = 100U, .area = 16.0F, .box = {0.0F, 0.0F, 1.0F, 1.0F}, .has_mask = true};
    }
    for (const bool geometry : {false, true}) {
        auto config = test_support::spatial_occlusion_config(geometry);
        config.copy_paste_probability = geometry ? 1.0F : 0.0F;
        test_support::AugmentationExecution execution_executor(0);
        GpuAugmentationExecutor executor(config, count, 4, 4, execution_executor.context, execution_executor.retirement);
        const std::vector<float> input(count * 48U, 0.9F);
        const auto output =
            execute_and_copy(executor, input, indices, keys, donors, std::vector<float>(input.size(), 0.7F), std::vector<std::int64_t>(count, 0xFFFF));
        const auto display =
            execute_and_copy(executor, input, indices, keys, donors, std::vector<float>(input.size(), 0.7F), std::vector<std::int64_t>(count, 0xFFFF), {},
                             GpuAugmentationDonorSelection::Aligned, GpuAugmentationOutputDomain::UnitRgb);
        check_model_normalized_rgb(output, display, 16U);
        std::size_t dropout = 0U;
        std::size_t rectangular = 0U;
        std::size_t channel_only = 0U;
        std::size_t erased = 0U;
        std::size_t erased_donor = 0U;
        for (std::size_t image = 0U; image < count; ++image) {
            const auto& plan = executor.plan().images[image];
            dropout += plan.erasure.dropout_probability > 0.0F;
            rectangular += plan.erasure.rectangular != 0U;
            channel_only += plan.erasure.dropout_probability == 0.0F && plan.erasure.rectangular == 0U;
            for (int y = 0; y < 4; ++y) {
                for (int x = 0; x < 4; ++x) {
                    const auto pixel = static_cast<std::size_t>(y * 4 + x);
                    const bool removed = augment_math::erases_pixel(plan.erasure, x, y, 4, 4);
                    const bool zero =
                        output[image * 48U + pixel] == 0.0F && output[image * 48U + 16U + pixel] == 0.0F && output[image * 48U + 32U + pixel] == 0.0F;
                    const float nx = (static_cast<float>(x) + 0.5F) / 4.0F;
                    const float ny = (static_cast<float>(y) + 0.5F) / 4.0F;
                    const float sx = plan.inverse[0] * nx + plan.inverse[1] * ny + plan.inverse[2];
                    const float sy = plan.inverse[3] * nx + plan.inverse[4] * ny + plan.inverse[5];
                    const float dx = plan.paste_inverse[0] * nx + plan.paste_inverse[2];
                    const float dy = plan.paste_inverse[4] * ny + plan.paste_inverse[5];
                    const bool donor = plan.paste_donor_slot >= 0 && dx >= 0.0F && dx <= 1.0F && dy >= 0.0F && dy <= 1.0F;
                    const bool content = donor || (sx >= 0.0F && sx <= 1.0F && sy >= 0.0F && sy <= 1.0F);
                    if (content) {
                        CHECK(zero == removed);
                        erased += removed;
                        erased_donor += removed && donor;
                    }
                }
            }
        }
        CHECK(dropout > 0U);
        CHECK(rectangular > 0U);
        CHECK(channel_only > 0U);
        CHECK(erased > 0U);
        if (geometry) { CHECK(erased_donor > 0U); }
        executor.Reconfigure(disabled_config());
        const auto restored = execute_and_copy(executor, input, indices, keys);
        CHECK_FALSE(executor.plan().erases_spatial_support);
        CHECK(std::ranges::none_of(restored, [](const float value) { return value == 0.0F; }));
    }
}
TEST_CASE("cached donors preserve the first eligible circular candidate", "[backend][augmentation][donors]") {
    detail::CachedAugmentationDonorIndex index;
    for (const std::vector<int>& catalog :
         {std::vector<int>{}, {-1}, {0}, {0, 0, 0}, {-1, -1, -1}, {0, -1, 0, 1, -1, 1, 0}, {2, 2, 1, 1, 0, 0, 2}, {-1, 3, -1, 0, -1, 3}}) {
        std::vector<GpuAugmentationDonor> donors(catalog.size());
        for (std::size_t slot = 0; slot < catalog.size(); ++slot) {
            donors[slot].label = catalog[slot] < 0 ? -1 : 0;
            donors[slot].dataset_index = static_cast<std::uint32_t>(std::max(catalog[slot], 0));
        }
        index.rebuild(donors);
        CHECK(index.select(catalog.size(), 0U) == -1);
        for (std::size_t start = 0; start < donors.size(); ++start)
            for (std::uint32_t source = 0; source < 5U; ++source) {
                std::int64_t expected = -1;
                for (std::size_t probe = 0; probe < donors.size(); ++probe) {
                    const auto candidate = (start + probe) % donors.size();
                    if (donors[candidate].label >= 0 && donors[candidate].dataset_index != source) {
                        expected = static_cast<std::int64_t>(candidate);
                        break;
                    }
                }
                CHECK(index.select(start, source) == expected);
            }
    }
}
TEST_CASE("paste admission handles disabled zero and certain probabilities", "[backend][augmentation][donors]") {
    CHECK(GpuAugmentationConfig{}.copy_paste_probability == .80F);
    auto config = test_support::isolated_augmentation_config();
    for (std::uint64_t key = 0; key < 128U; ++key) {
        config.copy_paste_probability = 0.0F;
        CHECK_FALSE(augmentation_paste_admitted(config, key));
        config.copy_paste_probability = 1.0F;
        CHECK(augmentation_paste_admitted(config, key));
        config.enabled = false;
        CHECK_FALSE(augmentation_paste_admitted(config, key));
        config.enabled = true;
    }
}
TEST_CASE("augmentation failed staging finish and reconfigure settlement closes exact owner admission", "[backend][augmentation][perceptual][cuda][custody]") {
    if (!has_cuda_device()) SKIP("CUDA unavailable; settlement custody case unexecuted");
    const auto failure_path = GENERATE(0, 1, 2);
    test_support::AugmentationExecution execution;
    std::weak_ptr<const void> owner;
    std::weak_ptr<AugmentationPixels> allocations;
    {
        auto config = test_support::isolated_augmentation_config();
        config.perceptual_downscale = true;
        config.resize = {.probability = 1.F, .min_strength = 1.F, .max_strength = 1.F};
        GpuAugmentationExecutor executor(config, 20, 9, 9, execution.context, execution.retirement);
        auto pixels = std::make_shared<AugmentationPixels>(20U * 3U * 81U);
        cuda_require(cudaMemsetAsync(pixels->input.data(), 0, pixels->input.size_bytes(), pixels->stream));
        std::array<std::uint32_t, 20> indices{};
        std::array<std::uint64_t, 20> keys{};
        for (std::size_t i = 0; i < keys.size(); ++i) keys[i] = i + 1;
        GpuAugmentationBatchView batch{.input = pixels->input.data(),
                                       .output = pixels->output.data(),
                                       .image_indices = indices,
                                       .height = 9,
                                       .width = 9,
                                       .input_custody = pixels,
                                       .output_custody = pixels,
                                       .input_capacity_bytes = pixels->input.size_bytes(),
                                       .output_capacity_bytes = pixels->output.size_bytes()};
        auto invalid = batch;
        invalid.height = 8;
        REQUIRE_THROWS(executor.Run(invalid, keys, {}, {}, pixels->stream));
        CHECK(execution.retirement.admission_open());
        (void)executor.Run(batch, keys, {}, {}, pixels->stream);
        owner = test_support::GpuAugmentationTestAccess::Custody(executor);
        allocations = pixels;
        test_support::GpuAugmentationTestAccess::FailEventWait(executor);
        if (failure_path == 0) REQUIRE_THROWS(executor.Run(batch, keys, {}, {}, pixels->stream));
        if (failure_path == 1) REQUIRE_THROWS(executor.Finish());
        if (failure_path == 2) {
            config.enabled = false;
            REQUIRE_THROWS(executor.Reconfigure(config));
        }
        CHECK_FALSE(execution.retirement.admission_open());
        CHECK(execution.retirement.fact().first_failure == cudaErrorLaunchFailure);
        CHECK(execution.retirement.fact().occupancy == 1U);
        REQUIRE_THROWS(executor.Run(batch, keys, {}, {}, pixels->stream));
        REQUIRE_THROWS(executor.RunTraining(batch, 1, 0, 0, 0, {}, {}, pixels->stream));
        REQUIRE_THROWS(executor.Finish());
        REQUIRE_THROWS(executor.Reconfigure(config));
        REQUIRE_THROWS(executor.plan());
        REQUIRE_THROWS(executor.enabled());
        REQUIRE_THROWS(executor.workspace_capacity_bytes());
        batch.input_custody.reset();
        batch.output_custody.reset();
        invalid.input_custody.reset();
        invalid.output_custody.reset();
        pixels.reset();
        CHECK_FALSE(owner.expired());
        CHECK_FALSE(allocations.expired());
    }
    CHECK_FALSE(owner.expired());
    CHECK_FALSE(allocations.expired());
}
double sample_reference(const mmltk::backend::imaging::resample::test_perceptual::Image& image, double x, double y, unsigned channel) {
    constexpr double mean[]{.485, .456, .406};
    if (x < 0 || x > 1 || y < 0 || y > 1) return mean[channel];
    const auto width = image.layout.width, height = image.layout.height;
    const double px = std::clamp(x * width - .5, 0.0, double(width - 1)), py = std::clamp(y * height - .5, 0.0, double(height - 1));
    const unsigned x0 = static_cast<unsigned>(px), y0 = static_cast<unsigned>(py);
    const unsigned x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
    return std::lerp(std::lerp(image.at(x0, y0, channel), image.at(x1, y0, channel), px - x0),
                     std::lerp(image.at(x0, y1, channel), image.at(x1, y1, channel), px - x0), py - y0);
}
TEST_CASE("perceptual augmentation preserves plans and independently remaps mixed batches above custody capacity",
          "[backend][models][rfdetr][augmentation][perceptual][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA unavailable; perceptual augmentation acceptance is unexecuted");
    enum class Completion { Retry, Finish, Destroy };
    const auto completion = GENERATE(Completion::Retry, Completion::Finish, Completion::Destroy);
    constexpr int extent = 9;
    constexpr std::size_t count = 64, plane = extent * extent;
    namespace oracle = mmltk::backend::imaging::resample::test_perceptual;
    oracle::Image original(extent, extent, mmltk::backend::imaging::resample::RgbPixelFormat::PlanarUnitSrgbF32);
    original.fill(3);
    std::vector<float> input(count * 3U * plane);
    std::array<std::uint32_t, count> indices{};
    std::array<std::uint64_t, count> keys{};
    for (std::size_t image = 0; image != count; ++image) {
        indices[image] = static_cast<std::uint32_t>(image);
        keys[image] = image + 1;
        for (unsigned channel = 0; channel != 3; ++channel)
            for (unsigned y = 0; y != extent; ++y)
                for (unsigned x = 0; x != extent; ++x) input[(image * 3U + channel) * plane + y * extent + x] = static_cast<float>(original.at(x, y, channel));
    }
    auto config = test_support::isolated_augmentation_config();
    config.resize = {.probability = .7F, .min_strength = .8F, .max_strength = .8F};
    test_support::AugmentationExecution execution;
    auto executor_owner = std::make_unique<GpuAugmentationExecutor>(config, count, extent, extent, execution.context, execution.retirement);
    auto& executor = *executor_owner;
    const auto executor_custody = test_support::GpuAugmentationTestAccess::Custody(executor);
    REQUIRE(execution.retirement.fact().reservations == 1U);
    std::vector<mmltk::frameworks::gpu::TerminalCudaRetirementLease> pressure;
    while (auto lease = execution.retirement.Reserve()) pressure.push_back(std::move(*lease));
    REQUIRE_FALSE(pressure.empty());
    const auto pressured_reservations = execution.retirement.fact().reservations;
    REQUIRE(pressured_reservations == pressure.size() + 1U);
    const auto baseline_bytes = executor.workspace_capacity_bytes();
    // The unselected path still runs while the concrete retirement owner is full.
    const auto ordinary =
        execute_and_copy(executor, input, indices, keys, {}, {}, {}, {}, GpuAugmentationDonorSelection::Aligned, GpuAugmentationOutputDomain::UnitRgb, extent);
    const auto plans = executor.plan().images;
    REQUIRE(std::ranges::any_of(plans, [](const auto& plan) { return std::ceil(extent * std::sqrt(plan.area_scale)) < extent; }));
    CHECK(executor.workspace_capacity_bytes() == baseline_bytes);
    config.perceptual_downscale = true;
    config.enabled = false;
    executor.Reconfigure(config);
    CHECK(execute_and_copy(executor, input, indices, keys, {}, {}, {}, {}, GpuAugmentationDonorSelection::Aligned, GpuAugmentationOutputDomain::UnitRgb,
                           extent) == input);
    config.enabled = true;
    executor.Reconfigure(config);
    const auto enlarged = std::ranges::find_if(plans, [](const auto& plan) { return plan.area_scale > 1.0F; });
    REQUIRE(enlarged != plans.end());
    const auto enlarged_index = static_cast<std::size_t>(enlarged - plans.begin());
    const std::vector<float> single_input(input.begin(), input.begin() + 3U * plane);
    const auto single_output =
        execute_and_copy(executor, single_input, std::span{indices}.subspan(enlarged_index, 1U), std::span{keys}.subspan(enlarged_index, 1U), {}, {}, {}, {},
                         GpuAugmentationDonorSelection::Aligned, GpuAugmentationOutputDomain::UnitRgb, extent);
    CHECK(std::equal(single_output.begin(), single_output.end(), ordinary.begin() + enlarged_index * 3U * plane));
    CHECK(executor.workspace_capacity_bytes() == baseline_bytes);
    CHECK(execution.retirement.fact().reservations == pressured_reservations);
    {
        auto pixels = std::make_shared<AugmentationPixels>(input.size());
        std::weak_ptr<AugmentationPixels> allocations = pixels;
        const std::vector<float> unchanged(input.size(), -.25F);
        cuda_require(cudaMemcpy(pixels->input.data(), input.data(), pixels->input.size_bytes(), cudaMemcpyHostToDevice));
        cuda_require(cudaMemcpy(pixels->output.data(), unchanged.data(), pixels->output.size_bytes(), cudaMemcpyHostToDevice));
        GpuAugmentationBatchView batch{.input = pixels->input.data(),
                                       .output = pixels->output.data(),
                                       .image_indices = indices,
                                       .height = extent,
                                       .width = extent,
                                       .output_domain = GpuAugmentationOutputDomain::UnitRgb,
                                       .input_custody = pixels,
                                       .output_custody = pixels,
                                       .input_capacity_bytes = pixels->input.size_bytes(),
                                       .output_capacity_bytes = pixels->output.size_bytes()};
        REQUIRE_THROWS_WITH(executor.Run(batch, keys, {}, {}, pixels->stream), "terminal CUDA custody reservation refused before resource allocation");
        CHECK(executor.plan().images == plans);
        CHECK(executor.workspace_capacity_bytes() == baseline_bytes);
        CHECK(execution.retirement.admission_open());
        CHECK(execution.retirement.fact().first_failure == cudaSuccess);
        CHECK(execution.retirement.fact().occupancy == 0U);
        CHECK(execution.retirement.fact().reservations == pressured_reservations);
        std::vector<float> failed_output(input.size());
        cuda_require(cudaMemcpy(failed_output.data(), pixels->output.data(), pixels->output.size_bytes(), cudaMemcpyDeviceToHost));
        CHECK(failed_output == unchanged);
        batch.input_custody.reset();
        batch.output_custody.reset();
        pixels.reset();
        CHECK(allocations.expired());
    }
    if (completion != Completion::Retry) {
        if (completion == Completion::Finish) {
            REQUIRE_NOTHROW(executor.Finish());
            CHECK(execution.retirement.fact().reservations == pressured_reservations);
        }
        executor_owner.reset();
        CHECK(executor_custody.expired());
        CHECK(execution.retirement.admission_open());
        CHECK(execution.retirement.fact().occupancy == 0U);
        CHECK(execution.retirement.fact().reservations == pressure.size());
        pressure.clear();
        CHECK(execution.retirement.fact().reservations == 0U);
        return;
    }
    pressure.clear();
    REQUIRE(execution.retirement.fact().reservations == 1U);
    const auto filtered =
        execute_and_copy(executor, input, indices, keys, {}, {}, {}, {}, GpuAugmentationDonorSelection::Aligned, GpuAugmentationOutputDomain::UnitRgb, extent);
    REQUIRE(executor.plan().images == plans);
    std::size_t shrink = 0, enlarge = 0, identity = 0;
    for (std::size_t image = 0; image != count; ++image) {
        const auto& plan = plans[image];
        if (plan.area_scale >= 1.0F) {
            if (plan.area_scale == 1.0F)
                ++identity;
            else
                ++enlarge;
            CHECK(std::equal(filtered.begin() + image * 3U * plane, filtered.begin() + (image + 1U) * 3U * plane, ordinary.begin() + image * 3U * plane));
            continue;
        }
        ++shrink;
        const auto reduced_extent = static_cast<unsigned>(std::ceil(extent * std::sqrt(plan.area_scale)));
        const auto reduced = oracle::reference(original, reduced_extent, reduced_extent);
        for (unsigned y = 0; y != extent; ++y)
            for (unsigned x = 0; x != extent; ++x) {
                const double nx = (x + .5) / extent, ny = (y + .5) / extent;
                const double sx = plan.inverse[0] * nx + plan.inverse[1] * ny + plan.inverse[2];
                const double sy = plan.inverse[3] * nx + plan.inverse[4] * ny + plan.inverse[5];
                for (unsigned channel = 0; channel != 3; ++channel) {
                    const double expected = sample_reference(reduced, sx, sy, channel);
                    CHECK(std::abs(filtered[(image * 3U + channel) * plane + y * extent + x] - expected) < 2e-4);
                }
            }
    }
    CHECK(shrink > 16U);
    CHECK(enlarge > 0U);
    CHECK(identity > 0U);
    CHECK(executor.workspace_capacity_bytes() > baseline_bytes);
    const auto retained_bytes = executor.workspace_capacity_bytes();
    CHECK(execute_and_copy(executor, input, indices, keys, {}, {}, {}, {}, GpuAugmentationDonorSelection::Aligned, GpuAugmentationOutputDomain::UnitRgb,
                           extent) == filtered);
    CHECK(executor.workspace_capacity_bytes() == retained_bytes);
    CHECK(execution.retirement.fact().reservations == 2U);
    executor_owner.reset();
    CHECK(executor_custody.expired());
    CHECK(execution.retirement.admission_open());
    CHECK(execution.retirement.fact().occupancy == 0U);
    CHECK(execution.retirement.fact().reservations == 0U);
}
TEST_CASE("perceptual donor reductions preserve mask box and class support", "[backend][augmentation][perceptual][cuda]") {
    if (!has_cuda_device()) SKIP("CUDA unavailable; donor acceptance unexecuted");
    namespace oracle = mmltk::backend::imaging::resample::test_perceptual;
    constexpr std::size_t count = 32;
    constexpr unsigned extent = 4, plane = extent * extent;
    oracle::Image original(extent, extent, mmltk::backend::imaging::resample::RgbPixelFormat::PlanarUnitSrgbF32);
    original.fill(3);
    std::array<std::uint32_t, count> indices{};
    std::array<std::uint64_t, count> keys{};
    std::vector<GpuAugmentationDonor> donors(count);
    std::vector<float> boxes(count * 4U), donor(count * 3U * plane);
    std::vector<std::int64_t> masks(count, 0x0660);
    for (std::size_t image = 0; image != count; ++image) {
        indices[image] = static_cast<std::uint32_t>(image);
        keys[image] = image + 17U;
        donors[image] = {.label = 255,
                         .dataset_index = static_cast<std::uint32_t>(image + count),
                         .area = 4.F,
                         .box = {.25F, .25F, .75F, .75F},
                         .has_mask = image % 2U == 0U};
        std::copy(donors[image].box.begin(), donors[image].box.end(), boxes.begin() + image * 4U);
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned y = 0; y < extent; ++y)
                for (unsigned x = 0; x < extent; ++x) donor[(image * 3U + c) * plane + y * extent + x] = static_cast<float>(original.at(x, y, c));
    }
    auto config = test_support::isolated_augmentation_config(1.F);
    test_support::AugmentationExecution execution;
    GpuAugmentationExecutor executor(config, count, extent, extent, execution.context, execution.retirement);
    const std::vector<float> source(count * 3U * plane, .125F);
    const auto ordinary = execute_and_copy(executor, source, indices, keys, donors, donor, masks, boxes, GpuAugmentationDonorSelection::Aligned,
                                           GpuAugmentationOutputDomain::UnitRgb, extent);
    const auto plans = executor.plan().images;
    config.perceptual_downscale = true;
    executor.Reconfigure(config);
    const auto selected = execute_and_copy(executor, source, indices, keys, donors, donor, masks, boxes, GpuAugmentationDonorSelection::Aligned,
                                           GpuAugmentationOutputDomain::UnitRgb, extent);
    REQUIRE(executor.plan().images == plans);
    std::array<std::size_t, 2> observed_reduced{}, changed_pixels{}, outside_pixels{};
    for (std::size_t image = 0; image < count; ++image) {
        const auto& plan = plans[image];
        REQUIRE(plan.paste_donor_slot >= 0);
        CHECK(plan.paste_label == 255);
        const auto side = std::min(extent, static_cast<unsigned>(std::ceil(extent / plan.paste_inverse[0])));
        const auto reduced = side < extent ? oracle::reference(original, side, side) : original;
        const auto mode = donors[image].has_mask ? 0U : 1U;
        for (unsigned y = 0; y < extent; ++y)
            for (unsigned x = 0; x < extent; ++x) {
                const double nx = (x + .5) / extent, ny = (y + .5) / extent;
                const double sx = plan.paste_inverse[0] * nx + plan.paste_inverse[1] * ny + plan.paste_inverse[2];
                const double sy = plan.paste_inverse[3] * nx + plan.paste_inverse[4] * ny + plan.paste_inverse[5];
                bool supported = false;
                if (sx >= 0 && sx < 1 && sy >= 0 && sy < 1) {
                    const auto px = static_cast<unsigned>(sx * extent), py = static_cast<unsigned>(sy * extent);
                    supported = mode == 0 ? ((std::uint64_t(masks[image]) >> (py * extent + px)) & 1U) != 0 : sx >= .25 && sx <= .75 && sy >= .25 && sy <= .75;
                }
                if (!supported) ++outside_pixels[mode];
                if (supported && side < extent) ++observed_reduced[mode];
                for (unsigned c = 0; c < 3; ++c) {
                    const auto offset = (image * 3U + c) * plane + y * extent + x;
                    const double expected = supported ? sample_reference(reduced, sx, sy, c) : .125;
                    CHECK(std::abs(selected[offset] - expected) < 2e-4);
                    if (!supported) CHECK(selected[offset] == ordinary[offset]);
                    if (supported && side < extent && std::abs(selected[offset] - ordinary[offset]) > 1e-3) ++changed_pixels[mode];
                }
            }
    }
    for (unsigned mode = 0; mode < 2; ++mode) {
        CHECK(observed_reduced[mode] > 0);
        CHECK(changed_pixels[mode] > 0);  // Omitting donor preparation cannot satisfy this case.
        CHECK(outside_pixels[mode] > 0);
    }
}
TEST_CASE("Preview presence and raster bounds stay independent through geometry donors and erasure", "[backend][rfdetr][augmentation][support]") {
    using mmltk::backend::data::PackedInstance;
    using mmltk::backend::data::RLEPair;
    const std::array runs{RLEPair{0, 1}};
    std::array<PackedInstance, 3> instances{};
    for (auto& item : instances) {
        item.bbox_x1 = 4.25F;
        item.bbox_y1 = 1.25F;
        item.bbox_x2 = 7.75F;
        item.bbox_y2 = 3.75F;
    }
    instances[0].flags = instances[1].flags = mmltk::backend::data::kAnnotationMask;
    instances[0].mask_rle_pairs = 1;
    std::vector<AugmentationPreviewAnnotation> output;
    AugmentationImagePlan plan;
    const auto build = [&] { build_augmentation_preview_annotations(instances, nullptr, &plan, 8, 4, output, runs); };
    build();
    REQUIRE(output.size() == 3);
    CHECK(output[0].mask_present);
    CHECK(output[0].mask_bounds == std::array<float, 4>{0, 0, .125F, .25F});
    CHECK(output[1].mask_present);
    CHECK(output[1].mask_bounds == std::array<float, 4>{});
    CHECK_FALSE(output[2].mask_present);
    plan.forward = {-1, 0, 1, 0, 1, 0};
    plan.inverse = plan.forward;
    build();
    REQUIRE(output.size() == 3);
    CHECK(output[0].mask_bounds[0] <= .875F);
    CHECK(output[0].mask_bounds[2] == 1.0F);
    CHECK(output[0].mask_bounds[3] >= .25F);
    CHECK(output[0].box_xyxy == std::array<float, 4>{.25F / 8, 1.25F / 4, 3.75F / 8, 3.75F / 4});
    CHECK(output[1].mask_present);
    CHECK(output[1].mask_bounds == std::array<float, 4>{});
    plan = {};
    plan.erasure.rectangular = 1;
    plan.erasure.x1 = .125F;
    plan.erasure.y1 = .25F;
    build();
    CHECK(std::ranges::none_of(output, [](const auto& item) { return item.source_ordinal == 0; }));
    for (bool masked : {false, true}) {
        plan = {};
        plan.paste_donor_slot = 0;
        plan.paste_masked = masked;
        plan.paste_source_box = {.5F, .25F, 1, 1};
        plan.paste_output_box = plan.paste_source_box;
        plan.paste_support = runs.data();
        plan.paste_support_count = runs.size();
        build_augmentation_preview_annotations({}, &instances[0], &plan, 8, 4, output, {});
        REQUIRE(output.size() == 1);
        CHECK(output[0].mask_present == masked);
        if (masked) {
            CHECK(output[0].mask_bounds[0] == 0.0F);
            CHECK(output[0].mask_bounds[2] >= .125F);
            CHECK(output[0].mask_bounds[3] >= .25F);
        } else {
            CHECK(output[0].mask_bounds == std::array<float, 4>{});
        }
    }
}
TEST_CASE("continuous augmentation boxes survive independent and empty mask support", "[backend][rfdetr][augmentation][support]") {
    using mmltk::backend::data::PackedInstance;
    using mmltk::backend::data::RLEPair;
    PackedInstance annotation{.class_id = 0,
                              .flags = mmltk::backend::data::kAnnotationMask,
                              .bbox_x1 = 1.25F,
                              .bbox_y1 = 2.5F,
                              .bbox_x2 = 6.25F,
                              .bbox_y2 = 7.25F,
                              .mask_rle_offset = 0,
                              .mask_rle_pairs = 0};
    const std::array runs{RLEPair{27, 1}};
    for (const auto mask : {std::span<const RLEPair>{}, std::span<const RLEPair>{runs}}) {
        AugmentationImagePlan plan;
        const auto identity = map_augmentation_instance(annotation, 8, 8, &plan, mask);
        CHECK(identity.visible);
        CHECK(identity.output_box_xyxy == std::array<float, 4>{1.25F / 8, 2.5F / 8, 6.25F / 8, 7.25F / 8});
        CHECK(identity.output_area == (mask.empty() ? 0.F : 1.F));
        plan.forward = {-1, 0, 1, 0, 1, 0};
        plan.inverse = plan.forward;
        const auto flipped = map_augmentation_instance(annotation, 8, 8, &plan, mask);
        CHECK(flipped.visible);
        CHECK(flipped.output_box_xyxy == std::array<float, 4>{1.75F / 8, 2.5F / 8, 6.75F / 8, 7.25F / 8});
        plan.forward = {2, 0, -.5F, 0, 2, -.5F};
        plan.inverse = {.5F, 0, .25F, 0, .5F, .25F};
        const auto cropped = map_augmentation_instance(annotation, 8, 8, &plan, mask);
        CHECK(cropped.visible);
        CHECK(cropped.output_box_xyxy == std::array<float, 4>{0, .125F, 1, 1});
        plan.forward = {1, 0, .625F, 0, 1, 0};
        plan.inverse = {1, 0, -.625F, 0, 1, 0};
        const auto vanished_mask = map_augmentation_instance(annotation, 8, 8, &plan, mask);
        CHECK(vanished_mask.visible);
        CHECK(vanished_mask.output_box_xyxy == std::array<float, 4>{6.25F / 8, 2.5F / 8, 1, 7.25F / 8});
        CHECK(vanished_mask.output_area == 0.0F);
    }
}
}  // namespace
}  // namespace mmltk::backend::models::rfdetr
