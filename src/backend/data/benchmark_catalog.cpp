#include "detail/benchmark_catalog.h"
#include "src/backend/data/catalog/coco_catalog.h"
#include <cstdio>
#include <algorithm>
#include <stdexcept>
#include <span>
#include <string_view>
#include "src/backend/data/benchmark_dataset_compiler.h"
namespace mmltk::backend::data::benchmark_internal {
namespace {
constexpr auto kCocoMappings = [] {
    std::array<NumericCategoryMapping, catalog::kCocoCategories.size()> mappings{};
    for (std::size_t index = 0; index < mappings.size(); ++index) {
        const auto& category = catalog::kCocoCategories[index];
        mappings[index] = {category.source_id, static_cast<std::uint8_t>(index), category.name};
    }
    return mappings;
}();
constexpr std::array<NumericCategoryMapping, 102> kObjects365Mappings{{
    {1, 0, "Person"},
    {3, 56, "Chair"},
    {6, 2, "Car"},
    {9, 39, "Bottle"},
    {11, 41, "Cup"},
    {14, 26, "Handbag/Satchel"},
    {19, 73, "Book"},
    {22, 8, "Boat"},
    {25, 13, "Bench"},
    {26, 58, "Potted Plant"},
    {27, 45, "Bowl/Basin"},
    {31, 75, "Vase"},
    {35, 2, "SUV"},
    {36, 40, "Wine Glass"},
    {38, 62, "Moniter/TV"},
    {39, 24, "Backpack"},
    {40, 25, "Umbrella"},
    {41, 9, "Traffic Light"},
    {44, 27, "Tie"},
    {47, 1, "Bicycle"},
    {48, 56, "Stool"},
    {50, 2, "Van"},
    {51, 57, "Couch"},
    {56, 5, "Bus"},
    {57, 14, "Wild Bird"},
    {59, 3, "Motorcycle"},
    {62, 67, "Cell Phone"},
    {66, 7, "Truck"},
    {73, 8, "Sailboat"},
    {74, 63, "Laptop"},
    {76, 59, "Bed"},
    {79, 17, "Horse"},
    {82, 71, "Sink"},
    {83, 47, "Apple"},
    {85, 43, "Knife"},
    {88, 7, "Pickup Truck"},
    {89, 42, "Fork"},
    {93, 16, "Dog"},
    {94, 44, "Spoon"},
    {95, 74, "Clock"},
    {97, 19, "Cow"},
    {98, 55, "Cake"},
    {99, 60, "Dinning Table"},
    {100, 18, "Sheep"},
    {105, 49, "Orange/Tangerine"},
    {107, 66, "Keyboard"},
    {113, 46, "Banana"},
    {114, 35, "Baseball Glove"},
    {115, 4, "Airplane"},
    {116, 64, "Mouse"},
    {117, 6, "Train"},
    {119, 32, "Soccer"},
    {120, 30, "Skiboard"},
    {121, 28, "Luggage"},
    {127, 2, "Sports Car"},
    {128, 11, "Stop Sign"},
    {133, 65, "Remote"},
    {134, 72, "Refrigerator"},
    {135, 69, "Oven"},
    {137, 14, "Duck"},
    {138, 34, "Baseball Bat"},
    {140, 15, "Cat"},
    {142, 50, "Broccoli"},
    {144, 53, "Pizza"},
    {145, 20, "Elephant"},
    {146, 36, "Skateboard"},
    {147, 37, "Surfboard"},
    {151, 54, "Donut"},
    {152, 27, "Bow Tie"},
    {153, 51, "Carrot"},
    {154, 61, "Toilet"},
    {155, 33, "Kite"},
    {164, 68, "Microwave"},
    {165, 14, "Pigeon"},
    {166, 32, "Baseball"},
    {170, 76, "Scissors"},
    {174, 31, "Snowboard"},
    {177, 10, "Fire Hydrant"},
    {178, 32, "Basketball"},
    {179, 22, "Zebra"},
    {181, 23, "Giraffe"},
    {189, 7, "Fire Truck"},
    {193, 56, "Wheelchair"},
    {200, 7, "Heavy Truck"},
    {205, 38, "Tennis Racket"},
    {207, 32, "American Football"},
    {211, 32, "Tennis"},
    {212, 8, "Ship"},
    {220, 29, "Frisbee"},
    {222, 14, "Chicken"},
    {227, 79, "Toothbrush"},
    {240, 32, "Volleyball"},
    {242, 14, "Goose"},
    {248, 32, "Golf Ball"},
    {250, 12, "Parking meter"},
    {263, 14, "Swan"},
    {266, 48, "Sandwich"},
    {278, 70, "Toaster"},
    {296, 21, "Bear"},
    {309, 2, "Formula 1 "},
    {320, 14, "Parrot"},
    {329, 78, "Hair Dryer"},
}};
constexpr std::array<StringCategoryMapping, 132> kOpenImagesMappings{{
    {"/m/01g317", 0, "Person"},
    {"/m/01bl7v", 0, "Boy"},
    {"/m/05r655", 0, "Girl"},
    {"/m/04yx4", 0, "Man"},
    {"/m/03bt1vf", 0, "Woman"},
    {"/m/0199g", 1, "Bicycle"},
    {"/m/0k4j", 2, "Car"},
    {"/m/01lcw4", 2, "Limousine"},
    {"/m/0pg52", 2, "Taxi"},
    {"/m/0h2r6", 2, "Van"},
    {"/m/04_sv", 3, "Motorcycle"},
    {"/m/0cmf2", 4, "Fixed-wing aircraft"},
    {"/m/01bjv", 5, "Bus"},
    {"/m/07jdr", 6, "Train"},
    {"/m/07r04", 7, "Truck"},
    {"/m/019jd", 8, "Boat"},
    {"/m/01btn", 8, "Barge"},
    {"/m/0ph39", 8, "Canoe"},
    {"/m/02068x", 8, "Gondola"},
    {"/m/015qff", 9, "Traffic light"},
    {"/m/01pns0", 10, "Fire hydrant"},
    {"/m/02pv19", 11, "Stop sign"},
    {"/m/015qbp", 12, "Parking meter"},
    {"/m/0cvnqh", 13, "Bench"},
    {"/m/015p6", 14, "Bird"},
    {"/m/01f8m5", 14, "Blue jay"},
    {"/m/0ccs93", 14, "Canary"},
    {"/m/09b5t", 14, "Chicken"},
    {"/m/09ddx", 14, "Duck"},
    {"/m/09csl", 14, "Eagle"},
    {"/m/0f6wt", 14, "Falcon"},
    {"/m/0dbvp", 14, "Goose"},
    {"/m/012074", 14, "Magpie"},
    {"/m/05n4y", 14, "Ostrich"},
    {"/m/09d5_", 14, "Owl"},
    {"/m/0gv1x", 14, "Parrot"},
    {"/m/05z6w", 14, "Penguin"},
    {"/m/06j2d", 14, "Raven"},
    {"/m/0h23m", 14, "Sparrow"},
    {"/m/0dftk", 14, "Swan"},
    {"/m/0jly1", 14, "Turkey"},
    {"/m/01dy8n", 14, "Woodpecker"},
    {"/m/01yrx", 15, "Cat"},
    {"/m/0bt9lr", 16, "Dog"},
    {"/m/03k3r", 17, "Horse"},
    {"/m/07bgp", 18, "Sheep"},
    {"/m/01xq0k1", 19, "Cattle"},
    {"/m/0bwd_0j", 20, "Elephant"},
    {"/m/01dws", 21, "Bear"},
    {"/m/01dxs", 21, "Brown bear"},
    {"/m/03bj1", 21, "Panda"},
    {"/m/0633h", 21, "Polar bear"},
    {"/m/06l9r", 21, "Red panda"},
    {"/m/0898b", 22, "Zebra"},
    {"/m/03bk1", 23, "Giraffe"},
    {"/m/01940j", 24, "Backpack"},
    {"/m/0hnnb", 25, "Umbrella"},
    {"/m/080hkjn", 26, "Handbag"},
    {"/m/01rkbr", 27, "Tie"},
    {"/m/01s55n", 28, "Suitcase"},
    {"/m/02wmf", 29, "Flying disc"},
    {"/m/071p9", 30, "Ski"},
    {"/m/06__v", 31, "Snowboard"},
    {"/m/018xm", 32, "Ball (Object)"},
    {"/m/02ctlc", 32, "Cricket ball"},
    {"/m/01226z", 32, "Football"},
    {"/m/044r5d", 32, "Golf ball"},
    {"/m/0wdt60w", 32, "Rugby ball"},
    {"/m/05ctyq", 32, "Tennis ball"},
    {"/m/02rgn06", 32, "Volleyball (Ball)"},
    {"/m/02zt3", 33, "Kite"},
    {"/m/03g8mr", 34, "Baseball bat"},
    {"/m/03grzl", 35, "Baseball glove"},
    {"/m/06_fw", 36, "Skateboard"},
    {"/m/019w40", 37, "Surfboard"},
    {"/m/0h8my_4", 38, "Tennis racket"},
    {"/m/04dr76w", 39, "Bottle"},
    {"/m/09tvcd", 40, "Wine glass"},
    {"/m/02p5f1q", 41, "Coffee cup"},
    {"/m/02jvh9", 41, "Mug"},
    {"/m/0dt3t", 42, "Fork"},
    {"/m/04ctx", 43, "Knife"},
    {"/m/058qzx", 43, "Kitchen knife"},
    {"/m/0cmx8", 44, "Spoon"},
    {"/m/04kkgm", 45, "Bowl"},
    {"/m/03hj559", 45, "Mixing bowl"},
    {"/m/09qck", 46, "Banana"},
    {"/m/014j1m", 47, "Apple"},
    {"/m/0l515", 48, "Sandwich"},
    {"/m/0cdn1", 48, "Hamburger"},
    {"/m/06pcq", 48, "Submarine sandwich"},
    {"/m/0cyhj_", 49, "Orange (fruit)"},
    {"/m/0hkxq", 50, "Broccoli"},
    {"/m/0fj52s", 51, "Carrot"},
    {"/m/01b9xk", 52, "Hot dog"},
    {"/m/0663v", 53, "Pizza"},
    {"/m/0jy4k", 54, "Doughnut"},
    {"/m/0fszt", 55, "Cake"},
    {"/m/01mzpv", 56, "Chair"},
    {"/m/0fqt361", 56, "Stool"},
    {"/m/0qmmr", 56, "Wheelchair"},
    {"/m/02crq1", 57, "Couch"},
    {"/m/0703r8", 57, "Loveseat"},
    {"/m/03m3pdh", 57, "Sofa bed"},
    {"/m/026qbn5", 57, "Studio couch"},
    {"/m/03fp41", 58, "Houseplant"},
    {"/m/03ssj5", 59, "Bed"},
    {"/m/061hd_", 59, "Infant bed"},
    {"/m/0h8n5zk", 60, "Kitchen & dining room table"},
    {"/m/09g1w", 61, "Toilet"},
    {"/m/07c52", 62, "Television"},
    {"/m/02522", 62, "Computer monitor"},
    {"/m/01c648", 63, "Laptop"},
    {"/m/020lf", 64, "Computer mouse"},
    {"/m/0qjjc", 65, "Remote control"},
    {"/m/01m2v", 66, "Computer keyboard"},
    {"/m/050k8", 67, "Mobile phone"},
    {"/m/0fx9l", 68, "Microwave oven"},
    {"/m/029bxz", 69, "Oven"},
    {"/m/01k6s3", 70, "Toaster"},
    {"/m/0130jx", 71, "Sink"},
    {"/m/040b_t", 72, "Refrigerator"},
    {"/m/0bt_c3", 73, "Book"},
    {"/m/01x3z", 74, "Clock"},
    {"/m/046dlr", 74, "Alarm clock"},
    {"/m/06_72j", 74, "Digital clock"},
    {"/m/0h8mzrc", 74, "Wall clock"},
    {"/m/02s195", 75, "Vase"},
    {"/m/01lsmm", 76, "Scissors"},
    {"/m/0kmg4", 77, "Teddy bear"},
    {"/m/03wvsk", 78, "Hair dryer"},
    {"/m/012xff", 79, "Toothbrush"},
}};
constexpr std::array<std::uint64_t, 51> kObjects365TrainShardSizes{
    3850237397ULL, 3835218513ULL, 3851635927ULL, 3698690868ULL, 3706974043ULL, 3668334883ULL, 3683660250ULL, 3668268422ULL, 3686130235ULL,
    3726836684ULL, 3665281260ULL, 3781111049ULL, 4193078792ULL, 4139575052ULL, 4166963795ULL, 3471483977ULL, 8305870133ULL, 8196642348ULL,
    8239012244ULL, 8158257134ULL, 8354168935ULL, 8393306544ULL, 8371666904ULL, 8403369737ULL, 8191360061ULL, 8211621520ULL, 8088005702ULL,
    8149828852ULL, 7948184422ULL, 8423120986ULL, 8200847460ULL, 8230494213ULL, 8219769302ULL, 8225131209ULL, 8355559259ULL, 8266860867ULL,
    8304567926ULL, 8140732124ULL, 8318395028ULL, 8342761395ULL, 8209327654ULL, 8253861700ULL, 8287788214ULL, 8497016986ULL, 8460489730ULL,
    4656815126ULL, 9579677223ULL, 8857804362ULL, 9075419050ULL, 9239286405ULL, 8766215730ULL,
};
// These immutable descriptors contain string literals only and are required by
// the noexcept catalog accessors for the lifetime of the process.
// NOLINTBEGIN(bugprone-throwing-static-initialization)
const CatalogArtifact kCocoAnnotations{
    "coco-2017-annotations", "http://images.cocodataset.org/annotations/annotations_trainval2017.zip", "annotations_trainval2017.zip", 252907541U, "",
};
const CatalogArtifact kCocoTrain{
    "coco-2017-train-images", "http://images.cocodataset.org/zips/train2017.zip", "train2017.zip", 19336861798ULL, "",
};
const CatalogArtifact kCocoVal{
    "coco-2017-val-images", "http://images.cocodataset.org/zips/val2017.zip", "val2017.zip", 815585330U, "",
};
const CatalogArtifact kObjectsAnnotations{
    "objects365-v2-train-annotations",
    "https://dorc.ks3-cn-beijing.ksyun.com/data-set/2020Objects365%E6%95%B0%E6%8D%AE%E9%9B%86/train/"
    "zhiyuan_objv2_train.tar.gz",
    "zhiyuan_objv2_train.tar.gz",
    1336483164U,
    "",
    BenchmarkDatasetSource::kObjects365V2,
};
const CatalogArtifact kOpenImagesBoxes{
    "open-images-v7-train-boxes",
    "https://storage.googleapis.com/openimages/v6/oidv6-train-annotations-bbox.csv",
    "oidv6-train-annotations-bbox.csv",
    2258447590U,
    "",
    BenchmarkDatasetSource::kOpenImagesV7,
};
const CatalogArtifact kOpenImagesClasses{
    "open-images-v7-boxable-classes",
    "https://storage.googleapis.com/openimages/v7/oidv7-class-descriptions-boxable.csv",
    "oidv7-class-descriptions-boxable.csv",
    12064U,
    "",
    BenchmarkDatasetSource::kOpenImagesV7,
};
// NOLINTEND(bugprone-throwing-static-initialization)
constexpr std::string_view kOpenImagesTrainImageUrlTemplate = "https://open-images-dataset.s3.amazonaws.com/train/{image_id_hex}.jpg";
}  // namespace
const std::array<std::string_view, 80>& coco80_class_names() noexcept { return catalog::kCocoNames; }
std::span<const NumericCategoryMapping> coco_category_mappings() noexcept { return kCocoMappings; }
std::span<const NumericCategoryMapping> objects365_category_mappings() noexcept { return kObjects365Mappings; }
std::span<const StringCategoryMapping> open_images_category_mappings() noexcept { return kOpenImagesMappings; }
const CatalogArtifact& coco_annotations_artifact() noexcept { return kCocoAnnotations; }
const CatalogArtifact& coco_train_images_artifact() noexcept { return kCocoTrain; }
const CatalogArtifact& coco_val_images_artifact() noexcept { return kCocoVal; }
const CatalogArtifact& objects365_annotations_artifact() noexcept { return kObjectsAnnotations; }
std::vector<CatalogArtifact> objects365_train_image_artifacts() {
    constexpr std::string_view kTrainBase = "https://dorc.ks3-cn-beijing.ksyun.com/data-set/2020Objects365%E6%95%B0%E6%8D%AE%E9%9B%86/train/";
    std::vector<CatalogArtifact> artifacts;
    artifacts.reserve(51U);
    for (std::uint32_t shard = 0U; shard < 51U; ++shard) {
        const std::string filename = "patch" + std::to_string(shard) + ".tar.gz";
        artifacts.push_back(CatalogArtifact{
            "objects365-v2-train-patch-" + std::to_string(shard),
            std::string(kTrainBase) + filename,
            filename,
            kObjects365TrainShardSizes[shard],
            "",
            BenchmarkDatasetSource::kObjects365V2,
        });
    }
    return artifacts;
}
const CatalogArtifact& open_images_boxes_artifact() noexcept { return kOpenImagesBoxes; }
const CatalogArtifact& open_images_classes_artifact() noexcept { return kOpenImagesClasses; }
std::string_view open_images_train_image_url_template() noexcept { return kOpenImagesTrainImageUrlTemplate; }
std::string open_images_train_image_url(const std::uint64_t image_id) {
    std::array<char, 17> encoded{};
    (void)std::snprintf(encoded.data(), encoded.size(), "%016llx", static_cast<unsigned long long>(image_id));
    return "https://open-images-dataset.s3.amazonaws.com/train/" + std::string(encoded.data()) + ".jpg";
}
struct BenchmarkSourceDescriptor {
    std::string_view name;
    std::string_view version;
};
inline constexpr std::array kBenchmarkSourceDescriptors{
    BenchmarkSourceDescriptor{"coco", "2017"},
    BenchmarkSourceDescriptor{"objects365", "v2-2020"},
    BenchmarkSourceDescriptor{"open-images", "v7-bboxes-v6"},
    BenchmarkSourceDescriptor{"coconut", "cvpr2024"},
    BenchmarkSourceDescriptor{"objects365-v1", "v1"},
};
[[nodiscard]] const BenchmarkSourceDescriptor* benchmark_source_descriptor(const BenchmarkDatasetSource source) noexcept {
    const std::size_t index = static_cast<std::size_t>(source);
    return index < kBenchmarkSourceDescriptors.size() ? &kBenchmarkSourceDescriptors[index] : nullptr;
}
std::string_view benchmark_source_name(const BenchmarkDatasetSource source) noexcept {
    const BenchmarkSourceDescriptor* descriptor = benchmark_source_descriptor(source);
    return descriptor != nullptr ? descriptor->name : "unknown";
}
std::string_view benchmark_source_version(const BenchmarkDatasetSource source) noexcept {
    const BenchmarkSourceDescriptor* descriptor = benchmark_source_descriptor(source);
    return descriptor != nullptr ? descriptor->version : "unknown";
}
CategoryLookup make_numeric_lookup(const std::span<const NumericCategoryMapping> mappings) {
    std::uint32_t maximum = 0U;
    for (const NumericCategoryMapping& mapping : mappings) { maximum = std::max(maximum, mapping.source_id); }
    CategoryLookup lookup;
    lookup.target_by_id.assign(static_cast<std::size_t>(maximum) + 1U, -1);
    lookup.expected_names.reserve(mappings.size());
    for (const NumericCategoryMapping& mapping : mappings) {
        if (mapping.target_id >= coco80_class_names().size() || mapping.source_id == 0U || lookup.target_by_id[mapping.source_id] != -1 ||
            !lookup.expected_names.emplace(mapping.source_id, mapping.expected_name).second) {
            throw std::runtime_error("benchmark numeric category mapping is invalid");
        }
        lookup.target_by_id[mapping.source_id] = mapping.target_id;
    }
    return lookup;
}
void NumericCategoryAdmission::observe(std::optional<std::uint32_t> id, std::optional<std::string_view> name) {
    const auto expected = lookup_.expected_names.find(id.value_or(0U));
    if (expected == lookup_.expected_names.end()) return;
    if (!id || !name || *name != expected->second) {
        throw std::runtime_error("benchmark source category metadata disagrees with fixed mapping for id " + std::to_string(id.value_or(0U)) + ": expected '" +
                                 std::string(expected->second) + "', found '" + std::string(name.value_or(std::string_view{})) + "'");
    }
    if (!matched_.emplace(*id).second) throw std::runtime_error("benchmark source category metadata repeats mapped id " + std::to_string(*id));
}
void NumericCategoryAdmission::complete() const {
    if (matched_.size() != lookup_.expected_names.size()) throw std::runtime_error("benchmark source is missing required mapped categories");
}
}  // namespace mmltk::backend::data::benchmark_internal
