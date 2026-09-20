#include "detail/coconut_annotations.h"
#include "src/backend/data/benchmark_dataset_options.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>
#include <archive.h>
#include <archive_entry.h>
#include <catch2/catch_test_macros.hpp>
#include <stb_image_write.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using namespace mmltk::backend::data;
using namespace mmltk::backend::data::benchmark_internal;
namespace {
using Json = nlohmann::json;
using mmltk::testsupport::ScopedTempDir;
void arrow_ok(const arrow::Status& status) { if (!status.ok()) throw std::runtime_error(status.ToString()); }
template<class T> T arrow_value(arrow::Result<T> value) { arrow_ok(value.status()); return std::move(value).ValueOrDie(); }
std::string png(int width, int height, std::span<const std::uint32_t> ids) {
    REQUIRE(ids.size() == static_cast<std::size_t>(width * height));
    std::vector<unsigned char> pixels(ids.size() * 3);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        pixels[i * 3] = ids[i] & 255; pixels[i * 3 + 1] = (ids[i] >> 8) & 255; pixels[i * 3 + 2] = (ids[i] >> 16) & 255;
    }
    std::string encoded;
    REQUIRE(stbi_write_png_to_func([](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), size); },
        &encoded, width, height, 3, pixels.data(), width * 3) != 0);
    return encoded;
}
void tar(const std::filesystem::path& path, std::span<const std::pair<std::string, std::string>> members, bool symlink = false) {
    std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    REQUIRE(archive_write_set_format_pax_restricted(writer.get()) == ARCHIVE_OK);
    REQUIRE(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
    for (const auto& [name, bytes] : members) {
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        archive_entry_set_pathname(entry.get(), name.c_str()); archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_filetype(entry.get(), symlink ? AE_IFLNK : AE_IFREG);
        if (symlink) archive_entry_set_symlink(entry.get(), "other");
        archive_entry_set_size(entry.get(), symlink ? 0 : bytes.size());
        REQUIRE(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
        if (!symlink) REQUIRE(archive_write_data(writer.get(), bytes.data(), bytes.size()) == static_cast<la_ssize_t>(bytes.size()));
    }
    REQUIRE(archive_write_close(writer.get()) == ARCHIVE_OK);
}
Json category_catalog() {
    Json rows = Json::array();
    // Literal external COCO metadata: independent of the production category mapping.
    constexpr std::pair<unsigned, const char*> categories[]{
        {1,"person"}, {2,"bicycle"}, {3,"car"}, {4,"motorcycle"}, {5,"airplane"},
        {6,"bus"}, {7,"train"}, {8,"truck"}, {9,"boat"}, {10,"traffic light"},
        {11,"fire hydrant"}, {13,"stop sign"}, {14,"parking meter"}, {15,"bench"},
        {16,"bird"}, {17,"cat"}, {18,"dog"}, {19,"horse"}, {20,"sheep"}, {21,"cow"},
        {22,"elephant"}, {23,"bear"}, {24,"zebra"}, {25,"giraffe"}, {27,"backpack"},
        {28,"umbrella"}, {31,"handbag"}, {32,"tie"}, {33,"suitcase"}, {34,"frisbee"},
        {35,"skis"}, {36,"snowboard"}, {37,"sports ball"}, {38,"kite"}, {39,"baseball bat"},
        {40,"baseball glove"}, {41,"skateboard"}, {42,"surfboard"}, {43,"tennis racket"},
        {44,"bottle"}, {46,"wine glass"}, {47,"cup"}, {48,"fork"}, {49,"knife"},
        {50,"spoon"}, {51,"bowl"}, {52,"banana"}, {53,"apple"}, {54,"sandwich"},
        {55,"orange"}, {56,"broccoli"}, {57,"carrot"}, {58,"hot dog"}, {59,"pizza"},
        {60,"donut"}, {61,"cake"}, {62,"chair"}, {63,"couch"}, {64,"potted plant"},
        {65,"bed"}, {67,"dining table"}, {70,"toilet"}, {72,"tv"}, {73,"laptop"},
        {74,"mouse"}, {75,"remote"}, {76,"keyboard"}, {77,"cell phone"}, {78,"microwave"},
        {79,"oven"}, {80,"toaster"}, {81,"sink"}, {82,"refrigerator"}, {84,"book"},
        {85,"clock"}, {86,"vase"}, {87,"scissors"}, {88,"teddy bear"}, {89,"hair drier"},
        {90,"toothbrush"}
    };
    for (const auto& [id,name] : categories) rows.push_back({{"id",id},{"name",name},{"isthing",0}});
    rows.push_back({{"id",200},{"name","unrelated stuff"},{"isthing",1}});
    return rows;
}
void json_file(const std::filesystem::path& path, Json value) {
    if (value.contains("images") && value.contains("annotations") && !value.contains("categories")) value["categories"] = category_catalog();
    mmltk::testsupport::write_text_file(path, value.dump());
}
Json segment(unsigned id = 1, unsigned category = 1, bool thing = true) {
    return {{"id", id}, {"category_id", category}, {"isthing", thing ? 1 : 0}, {"iscrowd", 0}, {"area", nullptr}};
}
std::shared_ptr<arrow::DataType> segment_type(bool integer_area) {
    return arrow::struct_({arrow::field("area", integer_area ? arrow::int64() : arrow::float64()), arrow::field("category_id", arrow::int64()),
        arrow::field("id", arrow::int64()), arrow::field("iscrowd", arrow::int64()), arrow::field("isthing", arrow::int64())});
}
std::shared_ptr<arrow::Schema> hf_schema(bool integer_area = false) {
    return arrow::schema({arrow::field("mask", arrow::struct_({arrow::field("bytes", arrow::binary()), arrow::field("path", arrow::utf8())})),
        arrow::field("segments_info", arrow::struct_({arrow::field("file_name", arrow::utf8()), arrow::field("image_id", arrow::int64()),
            arrow::field("segments_info", arrow::list(segment_type(integer_area)))})),
        arrow::field("image_info", arrow::struct_({arrow::field("coco_url", arrow::utf8()), arrow::field("date_captured", arrow::utf8()),
            arrow::field("file_name", arrow::utf8()), arrow::field("height", arrow::int64()), arrow::field("id", arrow::int64()),
            arrow::field("license", arrow::int64()), arrow::field("width", arrow::int64())}))});
}
void append_value(arrow::ArrayBuilder& builder, const Json& value) {
    if (value.is_null()) { arrow_ok(builder.AppendNull()); return; }
    switch (builder.type()->id()) {
        case arrow::Type::STRUCT: {
            auto& typed = static_cast<arrow::StructBuilder&>(builder);
            arrow_ok(typed.Append());
            const auto& type = static_cast<const arrow::StructType&>(*builder.type());
            for (int i = 0; i < type.num_fields(); ++i) append_value(*typed.field_builder(i), value.at(type.field(i)->name()));
            break;
        }
        case arrow::Type::LIST: {
            auto& typed = static_cast<arrow::ListBuilder&>(builder); arrow_ok(typed.Append());
            for (const auto& entry : value) append_value(*typed.value_builder(), entry);
            break;
        }
        case arrow::Type::INT64: arrow_ok(static_cast<arrow::Int64Builder&>(builder).Append(value.get<std::int64_t>())); break;
        case arrow::Type::DOUBLE: arrow_ok(static_cast<arrow::DoubleBuilder&>(builder).Append(value.get<double>())); break;
        case arrow::Type::STRING: arrow_ok(static_cast<arrow::StringBuilder&>(builder).Append(value.get<std::string>())); break;
        case arrow::Type::BINARY: arrow_ok(static_cast<arrow::BinaryBuilder&>(builder).Append(value.get<std::string>())); break;
        default: throw std::runtime_error("unsupported fixture type");
    }
}
void parquet_file(const std::filesystem::path& path, const Json& rows, std::shared_ptr<arrow::Schema> schema = hf_schema(),
    parquet::Compression::type codec = parquet::Compression::SNAPPY) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (const auto& field : schema->fields()) {
        std::unique_ptr<arrow::ArrayBuilder> builder;
        arrow_ok(arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &builder));
        for (const auto& row : rows) append_value(*builder, row.at(field->name()));
        std::shared_ptr<arrow::Array> column; arrow_ok(builder->Finish(&column)); columns.push_back(std::move(column));
    }
    auto output = arrow_value(arrow::io::FileOutputStream::Open(path.string()));
    parquet::WriterProperties::Builder properties;
    properties.compression(codec);
    arrow_ok(parquet::arrow::WriteTable(*arrow::Table::Make(schema, columns), arrow::default_memory_pool(), output, 1, properties.build()));
    arrow_ok(output->Close());
}
Json hf_row(unsigned id, std::string encoded, Json segments, int width = 3, int height = 3) {
    const auto digits = std::to_string(id);
    const auto stem = std::string(12U - digits.size(), '0') + digits;
    return {{"mask", {{"bytes", std::move(encoded)}, {"path", nullptr}}},
        {"segments_info", {{"file_name", stem + ".png"}, {"image_id", id}, {"segments_info", std::move(segments)}}},
        {"image_info", {{"coco_url", "http://images.cocodataset.org/train2017/" + stem + ".jpg"}, {"date_captured", "2017"},
            {"file_name", stem + ".jpg"}, {"height", height}, {"width", width}, {"id", id}, {"license", 1}}}};
}
CoconutPhysicalImage coco(unsigned id, CoconutImageNamespace source = CoconutImageNamespace::CocoTrain) {
    const auto digits = std::to_string(id);
    const auto prefix = source == CoconutImageNamespace::CocoTrain ? "train2017/" : source == CoconutImageNamespace::CocoUnlabeled ? "unlabeled2017/" : "val2017/";
    return {source, id, 0, prefix + std::string(12U - digits.size(), '0') + digits + ".jpg", "physical-coco-archive"};
}
CoconutPhysicalImage objects(unsigned id, CoconutImageNamespace source = CoconutImageNamespace::Objects365V2) {
    const auto digits = std::to_string(id);
    return {source, id, 32, std::string(source == CoconutImageNamespace::Objects365V1 ? "image/objects365_v1_" : "patch32/objects365_v2_") +
        std::string(8U - digits.size(), '0') + digits + ".jpg", "physical-objects-archive"};
}
CoconutImportRequest request(std::span<const CoconutPhysicalImage> physical, CoconutEdition edition = CoconutEdition::Base) {
    CoconutImportRequest value;
    value.edition = edition; value.input_identity = "pinned-local-fixture-inputs"; value.physical_images = physical;
    return value;
}
struct PollCancellation {
    mutable std::size_t polls=0;
    std::size_t stop_at=std::numeric_limits<std::size_t>::max();
    bool cancelled() const noexcept { return polls++ >= stop_at; }
};
std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void expect_runs(const CoconutComponent& component, std::span<const RLEPair> expected) {
    REQUIRE(component.index.mask_rle_pairs.size() == expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        CHECK(component.index.mask_rle_pairs[i].start == expected[i].start);
        CHECK(component.index.mask_rle_pairs[i].length == expected[i].length);
    }
}
}
TEST_CASE("COCONut release selection and fixed catalog are pinned", "[coconut]") {
    CHECK(BenchmarkDatasetSelection{}.dataset == BenchmarkDatasetVariant::CocoCustom);
    CHECK(BenchmarkDatasetSelection{}.validation == CoconutValidation::Coconut);
    CHECK_FALSE(valid_benchmark_selection({static_cast<BenchmarkDatasetVariant>(4), CoconutValidation::Coconut}));
    CHECK_FALSE(valid_benchmark_selection({BenchmarkDatasetVariant::Coconut, static_cast<CoconutValidation>(4)}));
    REQUIRE(coconut_release_catalog().size() == 5);
    const auto& base = coconut_release_component(CoconutEdition::Base);
    CHECK(base.expected_rows == 241602); REQUIRE(base.annotations.size() == 4);
    CHECK(base.annotations[0].expected_size == 417218921);
    CHECK(base.annotations[0].expected_sha256 == "cfddd65e62ffeafe32229a43f06c475facfb039d2107be02b380134cb4be38ae");
    CHECK(coconut_release_component(CoconutEdition::RelabeledValidation).expected_rows == 5000);
    CHECK(coconut_objects_training_artifacts(CoconutEdition::Large).size() == 4);
    CHECK(coconut_objects_training_artifacts(CoconutEdition::XLarge).size() == 7);
    CHECK(coconut_objects_training_artifacts(CoconutEdition::Large)[0].artifact_id == objects365_train_image_artifacts()[32].artifact_id);
    CHECK(coconut_unlabeled_images_artifact().expected_size == 20126613414ULL);
    CHECK(coconut_validation_images_artifact().expected_size == 5084467200ULL);
}
TEST_CASE("COCONut Parquet shards preserve exact holes singleton crowd null area and empty images", "[coconut]") {
    ScopedTempDir root("coconut-parquet");
    const std::array<std::uint32_t, 9> ids{1, 1, 1, 1, 2, 1, 1, 0, 1};
    auto first = segment(); first["iscrowd"] = 1;
    auto second = segment(2, 2); second["area"] = 17;
    const auto mask = png(3, 3, ids);
    auto row = hf_row(7, mask, Json::array({first, second}));
    const std::array<std::uint32_t, 9> void_ids{};
    auto empty = hf_row(8, png(3, 3, void_ids), Json::array());
    auto nonthing = segment(1, 1, false); nonthing["area"] = 0;
    const std::array<std::uint32_t, 9> filled{1,1,1,1,1,1,1,1,1};
    parquet_file(root.path() / "a.parquet", Json::array({row, empty}));
    parquet_file(root.path() / "b.parquet", Json::array({hf_row(9, png(3,3,filled), Json::array({nonthing}))}), hf_schema(true), parquet::Compression::ZSTD);
    const std::array physical{coco(7), coco(8, CoconutImageNamespace::CocoUnlabeled), coco(9)};
    auto input = request(physical); input.parquet_shards = {root.path() / "a.parquet", root.path() / "b.parquet"}; input.expected_rows = 3;
    auto components = import_coconut_annotations(input);
    REQUIRE(components.size() == 2);
    const auto& train = components[0];
    REQUIRE(train.index.images.size() == 2); REQUIRE(train.index.boxes.size() == 2);
    CHECK(train.index.images[0].source_image_id == 7); CHECK(train.index.images[1].source_image_id == 9); CHECK(train.index.images[1].box_count == 0);
    const auto& box = train.index.boxes[0];
    CHECK(box.original_area == 7); CHECK(box.x1 == 0); CHECK(box.y1 == 0); CHECK(box.x2 == 1); CHECK(box.y2 == 1);
    CHECK((box.flags & kAnnotationCrowd) != 0); CHECK(box.annotation_id == 1); CHECK(box.source_ordinal == 0); CHECK(box.class_id == 0);
    const auto& singleton = train.index.boxes[1];
    CHECK(singleton.x1 == 1.0F / 3.0F); CHECK(singleton.y1 == 1.0F / 3.0F); CHECK(singleton.x2 == 2.0F / 3.0F); CHECK(singleton.y2 == 2.0F / 3.0F);
    CHECK(singleton.original_area == 17); CHECK(singleton.source_ordinal == 1); CHECK(singleton.source_category_id == 2);
    const std::array<RLEPair, 4> runs{{{0,4}, {5,2}, {8,1}, {4,1}}}; expect_runs(train, runs);
    CHECK(components[1].source == CoconutImageNamespace::CocoUnlabeled); REQUIRE(components[1].index.images.size() == 1);
    CHECK(components[1].index.images[0].source_image_id == 8); CHECK(components[1].index.boxes.empty());
    const auto cache = root.path() / "index.bin";
    store_coconut_component(cache, train);
    auto loaded = load_coconut_component(cache, train.edition, train.source, input.input_identity);
    REQUIRE(loaded); CHECK(loaded->inventory == train.inventory); expect_runs(*loaded, runs);
    CHECK_FALSE(load_coconut_component(cache, train.edition, train.source, "another-edition"));
    auto manifest = read_json_file(cache.string() + ".complete.json");
    manifest["coconut"]["inventory_identity"] = std::string(64,'0');
    json_file(cache.string() + ".complete.json", manifest);
    CHECK_FALSE(load_coconut_component(cache, train.edition, train.source, input.input_identity));
}
TEST_CASE("COCONut Parquet rejects malformed nested records missing membership and bounded overflows", "[coconut]") {
    ScopedTempDir root("coconut-malformed");
    const std::array<std::uint32_t, 9> ids{1,1,1,1,1,1,1,1,1};
    auto row = hf_row(7, png(3,3,ids), Json::array({segment()}));
    const std::array physical{coco(7)};
    auto input = request(physical); input.parquet_shards = {root.path() / "a.parquet"}; input.expected_rows = 1;
    SECTION("null image metadata") { row["image_info"] = nullptr; }
    SECTION("null required segment ID") { row["segments_info"]["segments_info"][0]["id"] = nullptr; }
    SECTION("mismatched release IDs") { row["segments_info"]["image_id"] = 8; }
    SECTION("unknown thing category") { row["segments_info"]["segments_info"][0]["category_id"] = 12; }
    SECTION("contradictory dimensions") { row["image_info"]["width"] = 4; }
    SECTION("missing embedded bytes") { row["mask"]["bytes"] = nullptr; }
    SECTION("PNG admission") { input.limits.max_png_bytes = 1; }
    SECTION("pixel admission") { input.limits.max_pixels = 8; }
    SECTION("segment admission") { input.limits.max_segments = 0; }
    SECTION("release completeness") { input.expected_rows = 2; }
    SECTION("missing physical member") { input.physical_images = {}; }
    SECTION("undeclared void-independent RGB ID") { row["segments_info"]["segments_info"][0]["id"] = 2; }
    SECTION("duplicate segments") { row["segments_info"]["segments_info"].push_back(segment()); }
    parquet_file(input.parquet_shards[0], Json::array({row}));
    CHECK_THROWS(import_coconut_annotations(input));
}
TEST_CASE("COCONut Parquet cancellation and nested type validation occur before normalization", "[coconut]") {
    ScopedTempDir root("coconut-parquet-cancel");
    const std::array<std::uint32_t, 1> ids{1};
    auto row = hf_row(7, png(1,1,ids), Json::array({segment()}), 1, 1);
    const std::array physical{coco(7)};
    auto input = request(physical); input.parquet_shards = {root.path() / "a.parquet"};
    SECTION("wrong nested type") {
        auto schema = hf_schema();
        auto fields = schema->fields(); fields[0] = arrow::field("mask", arrow::utf8()); row["mask"] = "wrong";
        parquet_file(input.parquet_shards[0], Json::array({row}), arrow::schema(fields));
        CHECK_THROWS(import_coconut_annotations(input));
    }
    SECTION("between records") {
        parquet_file(input.parquet_shards[0], Json::array({row, hf_row(8, png(1,1,ids), Json::array({segment()}), 1, 1)}));
        std::atomic<bool> stop{false}; input.cancellation = mmltk::common::concurrency::CancellationObservation::Atomic(stop);
        unsigned observed = 0; input.progress = [&](std::uint64_t) { ++observed; stop = true; };
        CHECK_THROWS(import_coconut_annotations(input)); CHECK(observed == 1);
    }
}
TEST_CASE("COCONut JSON joins retain heterogeneous Large rows and validation physical namespaces", "[coconut]") {
    ScopedTempDir root("coconut-json");
    const std::array<std::uint32_t, 4> ids{1,0,0,1};
    const auto mask = png(2,2,ids);
    const std::array physical{objects(91105, CoconutImageNamespace::Objects365V1), objects(91105)};
    auto input = request(physical, CoconutEdition::ObjectsValidation);
    input.annotation_json = root.path() / "val.json"; input.mask_archive = root.path() / "val.tar"; input.expected_rows = 2;
    auto supported = segment(); supported["ignore"] = 1; supported["bbox"] = Json::array({0.25,0.5,1.5,1.0});
    Json document{{"images", Json::array({{{"id",691105}, {"file_name","691105.jpg"}, {"width",2}, {"height",2}},
        {{"id",91105}, {"file_name","objects365_v2_00091105.png"}, {"width",2}, {"height",2}}})},
        {"annotations", Json::array({{{"image_id",691105}, {"file_name","691105.png"}, {"object365_file_name","objects365_v1_00091105"}, {"segments_info",Json::array({supported})}},
        {{"image_id",91105}, {"file_name","objects365_v2_00091105.png"}, {"segments_info",Json::array({segment()})}}})}};
    json_file(input.annotation_json, document);
    // Deliberately reversed archive order.
    const std::array members{std::pair{"panoptic_o365val_v3/objects365_v2_00091105.png", mask},
        std::pair{"panoptic_o365val_v3/objects365_v1_00091105.png", mask}, std::pair{".DS_Store", std::string("unrelated")}};
    std::vector<std::pair<std::string,std::string>> owned;
    for (const auto& [name, bytes] : members) owned.emplace_back(name, bytes);
    tar(input.mask_archive, owned);
    auto components = import_coconut_annotations(input);
    REQUIRE(components.size() == 2);
    CHECK(components[0].source == CoconutImageNamespace::Objects365V1); CHECK(components[1].source == CoconutImageNamespace::Objects365V2);
    for (const auto& component : components) CHECK(component.index.images[0].source_image_id == 91105);
    CHECK(components[0].inventory[0].release_image_id == 691105);
    CHECK(components[0].inventory[0].physical.member == "image/objects365_v1_00091105.jpg");
    const auto& box = components[0].index.boxes[0];
    CHECK(box.x1 == 0.125F); CHECK(box.y1 == 0.25F); CHECK(box.x2 == 0.875F); CHECK(box.y2 == 0.75F);
    CHECK((box.flags & kAnnotationIgnore) != 0); CHECK(box.original_area == 2);
    const std::array<RLEPair, 2> runs{{{0,1},{3,1}}}; expect_runs(components[0], runs);
    CHECK(components[0].index.boxes[0].source_ordinal == 0); CHECK(components[1].index.boxes[0].source_ordinal == 1);
    const auto cache = root.path() / "val.bin";
    store_coconut_component(cache, components[0]);
    auto loaded = load_coconut_component(cache, input.edition, CoconutImageNamespace::Objects365V1, input.input_identity);
    REQUIRE(loaded); CHECK(loaded->inventory[0].release_image_id == 691105);
}
TEST_CASE("COCONut Large shapes and sorted XL masks retain complete rows with Large precedence", "[coconut]") {
    ScopedTempDir root("coconut-extensions");
    const std::array<std::uint32_t, 1> ids{1}; const auto mask = png(1,1,ids);
    const std::array physical{objects(1), objects(2), objects(3)};
    auto input = request(physical, CoconutEdition::Large); input.expected_rows = 2;
    input.annotation_json = root.path() / "large.json"; input.mask_archive = root.path() / "large.tar";
    json_file(input.annotation_json, {{"images", Json::array({{{"id",900}, {"file_name","900.jpg"}, {"object365_name","objects365_v2_00000002"}, {"width",1}, {"height",1}},
        {{"id",1}, {"file_name","objects365_v2_00000001.png"}, {"width",1}, {"height",1}}})},
        {"annotations", Json::array({{{"image_id",900}, {"file_name","900.png"}, {"segments_info",Json::array({segment()})}},
        {{"image_id",1}, {"file_name","objects365_v2_00000001.png"}, {"segments_info",Json::array({segment()})}}})}});
    const std::array<std::pair<std::string,std::string>,2> members{{{"panoptic_object365/objects365_v2_00000001.png",mask}, {"panoptic_object365/objects365_v2_00000002.png",mask}}};
    tar(input.mask_archive, members);
    auto components = import_coconut_annotations(input);
    REQUIRE(components.size() == 1); REQUIRE(components[0].index.images.size() == 2);
    CHECK(components[0].inventory[1].release_image_id == 900); CHECK(components[0].index.boxes[0].source_ordinal == 1); CHECK(components[0].index.boxes[1].source_ordinal == 0);
    input.edition = CoconutEdition::XLarge; input.annotation_json.clear(); input.mask_archive = root.path() / "xl.tar";
    const std::array<std::pair<std::string,std::string>,4> xl_members{{{"coconuts_xlarge/panseg/objects365_v2_00000003.png",mask},
        {"coconuts_xlarge/panseg_info/objects365_v2_00000003.json", Json::array({segment()}).dump()},
        {"coconuts_xlarge/panseg_info/objects365_v2_00000002.json", Json::array({segment()}).dump()},
        {"coconuts_xlarge/panseg/objects365_v2_00000002.png",mask}}};
    tar(input.mask_archive, xl_members);
    auto xl = import_coconut_annotations(input);
    REQUIRE(xl.size() == 1); REQUIRE(xl[0].index.images.size() == 2);
    CHECK(xl[0].index.images[0].source_image_id == 2); CHECK(xl[0].index.images[0].width == 1); CHECK(xl[0].index.images[0].height == 1);
    CHECK(xl[0].index.boxes[0].source_ordinal == 0); CHECK(xl[0].index.boxes[1].source_ordinal == 1);
    components.push_back(std::move(xl[0]));
    const auto original=components;
    const auto* retained_boxes = components[1].index.boxes.data();
    const auto* retained_runs = components[1].index.mask_rle_pairs.data();
    const auto* retained_inventory = components[1].inventory.data();
    const auto box_capacity = components[1].index.boxes.capacity(), run_capacity = components[1].index.mask_rle_pairs.capacity();
    const auto previous_identity = components[1].index.annotation_sha256;
    PollCancellation observed;
    CHECK(reconcile_coconut_extensions(components,mmltk::common::concurrency::CancellationObservation::Borrow(observed)) == 1);
    for (std::size_t cut=0; cut<observed.polls; ++cut) {
        auto interrupted=original;
        PollCancellation stop; stop.stop_at=cut;
        CHECK_THROWS(reconcile_coconut_extensions(interrupted,mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
        if (interrupted[1].index.annotation_sha256.empty()) {
            const auto rejected_path = root.path() / "interrupted.bin";
            CHECK_THROWS(store_coconut_component(rejected_path, interrupted[1]));
            CHECK_FALSE(std::filesystem::exists(rejected_path));
        }
    }
    CHECK(components[1].index.boxes.data() == retained_boxes); CHECK(components[1].index.mask_rle_pairs.data() == retained_runs);
    CHECK(components[1].inventory.data() == retained_inventory);
    CHECK(components[1].index.boxes.capacity() == box_capacity); CHECK(components[1].index.mask_rle_pairs.capacity() == run_capacity);
    CHECK(components[1].index.annotation_sha256 != previous_identity);
    CHECK(components[1].inventory[0] == original[1].inventory[1]);
    CHECK(components[1].index.mask_rle_pairs[0].start == 0); CHECK(components[1].index.mask_rle_pairs[0].length == 1);
    REQUIRE(components[1].index.images.size() == 1); CHECK(components[1].index.images[0].source_image_id == 3);
    CHECK(components[1].index.boxes[0].source_ordinal == 1); CHECK(components[1].index.images[0].first_box == 0);
    CHECK(components[1].index.boxes[0].mask_rle_offset == 0);
    CHECK(reconcile_coconut_extensions(components) == 0);
    CHECK(components[1].index.boxes.data() == retained_boxes);
    auto covered = original;
    covered[0] = original[1]; covered[0].edition = CoconutEdition::Large;
    CHECK(reconcile_coconut_extensions(covered) == 2);
    CHECK(covered[1].inventory.empty()); CHECK(covered[1].index.images.empty());
    CHECK(covered[1].index.boxes.empty()); CHECK(covered[1].index.mask_rle_pairs.empty());
    CHECK(covered[0].inventory.size() == 2);

}
TEST_CASE("COCONut archives reject unresolved duplicate extra and unsafe offered members", "[coconut]") {
    ScopedTempDir root("coconut-archive-reject");
    const std::array<std::uint32_t,1> ids{1}; const auto mask = png(1,1,ids);
    const std::array physical{objects(1)};
    auto input = request(physical, CoconutEdition::Large);
    input.annotation_json = root.path() / "large.json"; input.mask_archive = root.path() / "large.tar";
    Json annotation{{"image_id",1}, {"file_name","objects365_v2_00000001.png"}, {"segments_info",Json::array({segment()})}};
    Json document{{"images", Json::array()}, {"annotations", Json::array({annotation})}};
    std::vector<std::pair<std::string,std::string>> members{{"panoptic_object365/objects365_v2_00000001.png",mask}};
    bool link = false;
    SECTION("missing mask") { members.clear(); }
    SECTION("extra mask") { members.emplace_back("panoptic_object365/objects365_v2_00000002.png",mask); }
    SECTION("duplicate mask") { members.push_back(members.front()); }
    SECTION("traversal") { members.emplace_back("../outside", "x"); }
    SECTION("symlink") { link = true; }
    SECTION("duplicate offered annotation") { document["annotations"].push_back(annotation); }
    SECTION("unknown physical namespace") { document["annotations"][0]["file_name"] = "00000001.png"; }
    SECTION("contradictory physical names") { document["annotations"][0]["object365_file_name"] = "objects365_v2_00000002"; }
    SECTION("unknown PNG segment") { const std::array<std::uint32_t,1> unknown{2}; members[0].second = png(1,1,unknown); }
    SECTION("absent thing support") { const std::array<std::uint32_t,1> empty{0}; members[0].second = png(1,1,empty); }
    json_file(input.annotation_json, document); tar(input.mask_archive, members, link);
    CHECK_THROWS(import_coconut_annotations(input));
}
TEST_CASE("COCONut version-1 physical inventory has fixed bytes and admits existing caches", "[coconut]") {
    ScopedTempDir root("coconut-inventory-v1");
    static constexpr char expected_bytes[] =
        "\x43\x4e\x55\x54\x49\x56\x4e\x31"  // magic
        "\x01\x00\x00\x00"  // version 1
        "\x02\x00\x00\x00"  // cache schema 2
        "\x18\x00\x00\x00\x63\x6f\x63\x6f\x6e\x75\x74\x2d\x65\x78\x61\x63\x74\x2d\x72\x67\x62\x2d\x72\x6c\x65\x2d\x76\x31"  // normalization
        "\x01\x00\x00\x00\x61"  // input identity
        "\x00\x03\x34\x12\x00"  // Base, Objects365V1, shard 0x1234, physical
        "\x01\x00\x00\x00\x00\x00\x00\x00"  // one record
        "\x03\xe1\x63\x01\x00\x00\x00\x00\x00\x34\x12"  // namespace, physical ID 91105, shard
        "\x20\x00\x00\x00\x69\x6d\x61\x67\x65\x2f\x6f\x62\x6a\x65\x63\x74\x73\x33\x36\x35\x5f\x76\x31\x5f\x30\x30\x30\x39\x31\x31\x30\x35\x2e\x6a\x70\x67"  // physical member
        "\x01\x00\x00\x00\x61"  // archive identity
        "\x93\xe4\x7a\xfa\x75\xda\x69\x5b\x47\xe8\xc5\x40\x1f\xb2\x30\xea\xde\x72\x66\xf7\x98\xbc\xea\x05\x7b\x52\x8f\xbd\x0b\x94\x5e\x17";  // SHA-256 footer at byte 114
    const std::string expected(expected_bytes, sizeof(expected_bytes) - 1);
    const auto archive = root.path() / "images.tar";
    const auto cache = root.path() / "inventory.bin";
    const std::vector<CoconutPhysicalImage> wanted{{CoconutImageNamespace::Objects365V1, 91105, 0x1234,
        "image/objects365_v1_00091105.jpg", "a"}};
    // A preexisting v1 inventory must load without its source archive.
    mmltk::testsupport::write_text_file(cache, expected);
    CHECK(coconut_image_archive_inventory(archive, cache, CoconutImageNamespace::Objects365V1, 0x1234, "a") == wanted);
    CHECK_FALSE(std::filesystem::exists(archive));
    const std::array<std::pair<std::string, std::string>, 1> members{{{wanted[0].member, "jpeg"}}};
    tar(archive, members);
    const auto emitted = root.path() / "emitted.bin";
    CHECK(coconut_image_archive_inventory(archive, emitted, CoconutImageNamespace::Objects365V1, 0x1234, "a") == wanted);
    CHECK(file_bytes(emitted) == expected);
    CHECK(expected.size() == 114 + 32);
    CHECK(file_bytes(cache) == expected);
}
TEST_CASE("COCONut version-1 component inventory pins nested physical release and ordinal identities", "[coconut]") {
    ScopedTempDir root("coconut-component-v1");
    static constexpr char expected_bytes[] =
        "\x43\x4e\x55\x54\x49\x56\x4e\x31"  // magic
        "\x01\x00\x00\x00"  // version 1
        "\x02\x00\x00\x00"  // cache schema 2
        "\x18\x00\x00\x00\x63\x6f\x63\x6f\x6e\x75\x74\x2d\x65\x78\x61\x63\x74\x2d\x72\x67\x62\x2d\x72\x6c\x65\x2d\x76\x31"  // normalization
        "\x01\x00\x00\x00\x69"  // input identity
        "\x04\x03\x00\x00\x01"  // ObjectsValidation, Objects365V1, header shard 0, component
        "\x01\x00\x00\x00\x00\x00\x00\x00"  // one record
        "\x03\xe1\x63\x01\x00\x00\x00\x00\x00\x34\x12"  // namespace, physical ID 91105, shard
        "\x20\x00\x00\x00\x69\x6d\x61\x67\x65\x2f\x6f\x62\x6a\x65\x63\x74\x73\x33\x36\x35\x5f\x76\x31\x5f\x30\x30\x30\x39\x31\x31\x30\x35\x2e\x6a\x70\x67"  // physical member
        "\x01\x00\x00\x00\x61"  // archive identity
        "\xa1\x8b\x0a\x00\x00\x00\x00\x00"  // declared release ID 691105
        "\x02\x00\x00\x00\x00\x00\x00\x00"  // source ordinal 2
        "\xc5\x30\x4c\xa7\xa1\x6f\x7e\x76\xd6\xc2\x1b\xfd\x7b\xda\x2b\x39\xa0\x25\xcc\x26\x8d\xde\x23\x36\xd9\x5a\x21\x57\xeb\x9d\x46\x32";  // SHA-256 footer at byte 130
    const std::string expected(expected_bytes, sizeof(expected_bytes) - 1);
    CoconutComponent component;
    component.edition = CoconutEdition::ObjectsValidation;
    component.source = CoconutImageNamespace::Objects365V1;
    component.input_identity = "i";
    component.inventory = {{{CoconutImageNamespace::Objects365V1, 91105, 0x1234,
        "image/objects365_v1_00091105.jpg", "a"}, 691105, 2}};
    component.index.source = BenchmarkDatasetSource::kObjects365V2;
    component.index.split = "coconut-4-3";
    component.index.annotation_sha256 = "c5304ca7a16f7e76d6c21bfd7bda2b39a025cc268dde2336d95a2157eb9d4632";
    component.index.images.push_back({.source_image_id = 91105, .width = 1, .height = 1, .source_shard = 0x1234});
    const auto path = root.path() / "component.bin";
    store_coconut_component(path, component);
    const auto index_bytes = file_bytes(path);
    const auto inventory_path = path.string() + ".inventory";
    CHECK(file_bytes(inventory_path) == expected);
    CHECK(expected.size() == 130 + 32);
    // Substitute independent v1 bytes, retaining the index's joint completion.
    mmltk::testsupport::write_text_file(inventory_path, expected);
    const auto loaded = load_coconut_component(path, component.edition, component.source, "i");
    REQUIRE(loaded);
    CHECK(loaded->inventory == component.inventory);
    REQUIRE(loaded->index.images.size() == 1);
    CHECK(loaded->index.images[0].source_image_id == 91105);
    CHECK(loaded->index.boxes.empty());
    CHECK_FALSE(load_coconut_component(path, component.edition, component.source, "changed"));
    store_coconut_component(path, *loaded);
    CHECK(file_bytes(inventory_path) == expected);
    CHECK(file_bytes(path) == index_bytes);
}
TEST_CASE("COCONut full physical inventories are identity-bound and independent of foreground labels", "[coconut]") {
    ScopedTempDir root("coconut-inventory");
    const auto archive = root.path() / "images.tar", cache = root.path() / "inventory.json";
    const std::array<std::pair<std::string,std::string>,3> members{{{"unlabeled2017/000000000007.jpg","jpeg7"},
        {"unlabeled2017/000000000008.jpg","jpeg8"}, {".DS_Store","unrelated"}}};
    tar(archive, members);
    auto result = coconut_image_archive_inventory(archive, cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive");
    REQUIRE(result.size() == 2); CHECK(result[0].image_id == 7); CHECK(result[1].image_id == 8);
    std::filesystem::remove(archive);
    CHECK(coconut_image_archive_inventory(archive, cache, CoconutImageNamespace::CocoUnlabeled, 0, "pinned-archive") == result);
    CHECK_THROWS(coconut_image_archive_inventory(archive, cache, CoconutImageNamespace::CocoUnlabeled, 0, "changed-archive"));
    const std::array<std::pair<std::string,std::string>,2> duplicate{{members[0],members[0]}};
    tar(archive, duplicate);
    CHECK_THROWS(coconut_image_archive_inventory(archive, {}, CoconutImageNamespace::CocoUnlabeled, 0, "duplicate-archive"));
}
TEST_CASE("COCONut preserves RGB24 IDs across compressed Parquet batches and relabeled integer areas", "[coconut]") {
    ScopedTempDir root("coconut-rgb24");
    const std::array<std::uint32_t,1> ids{0x030201};
    auto seg = segment(0x030201, 90); seg["area"] = 1;
    Json rows = Json::array();
    std::vector<CoconutPhysicalImage> physical;
    for (unsigned i = 1; i <= 18; ++i) {
        rows.push_back(hf_row(i, png(1,1,ids), Json::array({seg}), 1, 1)); physical.push_back(coco(i, CoconutImageNamespace::CocoValidation));
    }
    auto input = request(physical, CoconutEdition::RelabeledValidation); input.parquet_shards = {root.path() / "val.parquet"}; input.expected_rows = 18;
    parquet_file(input.parquet_shards[0], rows, hf_schema(true), parquet::Compression::GZIP);
    auto components = import_coconut_annotations(input);
    REQUIRE(components.size() == 1); REQUIRE(components[0].index.images.size() == 18); REQUIRE(components[0].index.boxes.size() == 18);
    for (std::size_t i = 0; i < 18; ++i) {
        CHECK(components[0].index.boxes[i].annotation_id == 0x030201); CHECK(components[0].index.boxes[i].class_id == 79);
        CHECK(components[0].index.boxes[i].source_ordinal == i); CHECK(components[0].index.mask_rle_pairs[i].start == 0);
        CHECK(components[0].index.mask_rle_pairs[i].length == 1);
    }
}
TEST_CASE("COCONut native selection admission precedes cache mutation", "[coconut]") {
    ScopedTempDir root("coconut-selection-admission");
    BenchmarkCompilerConfig config;
    config.cache_dir = root.path() / "new-cache"; config.output_dir = root.path() / "new-output";
    config.selection.dataset = static_cast<BenchmarkDatasetVariant>(255);
    CHECK_THROWS_AS(compile_benchmark_dataset(config), std::invalid_argument);
    CHECK_FALSE(std::filesystem::exists(config.cache_dir)); CHECK_FALSE(std::filesystem::exists(config.output_dir));
}
TEST_CASE("COCONut duplicate and ambiguous B membership never silently loses rows", "[coconut]") {
    ScopedTempDir root("coconut-base-membership");
    const std::array<std::uint32_t,1> ids{1};
    auto row = hf_row(7, png(1,1,ids), Json::array({segment()}),1,1);
    std::vector<CoconutPhysicalImage> physical{coco(7)};
    Json rows = Json::array({row});
    SECTION("duplicate rows") { rows.push_back(row); }
    SECTION("ambiguous physical subsets") { physical.push_back(coco(7, CoconutImageNamespace::CocoUnlabeled)); }
    SECTION("duplicate physical members") { physical.push_back(physical[0]); }
    auto input = request(physical); input.parquet_shards = {root.path() / "b.parquet"};
    parquet_file(input.parquet_shards[0], rows);
    CHECK_THROWS(import_coconut_annotations(input));
}
TEST_CASE("COCONut authoritative box retains an empty supplied mask and rejects coordinate overflow", "[coconut]") {
    ScopedTempDir root("coconut-authoritative-box");
    const std::array<std::uint32_t,1> ids{0};
    const std::array physical{objects(1)};
    auto input = request(physical, CoconutEdition::Large);
    input.annotation_json = root.path() / "large.json"; input.mask_archive = root.path() / "large.tar";
    auto seg = segment(); seg["bbox"] = Json::array({-1,0,3,1}); seg["area"] = 23;
    bool overflow = false;
    SECTION("outside-image authoritative bounds") {}
    SECTION("unrepresentable bounds") { seg["bbox"] = Json::array({1e30,0,1,1}); overflow = true; }
    json_file(input.annotation_json, {{"images",Json::array()}, {"annotations",Json::array({{{"image_id",1},
        {"file_name","objects365_v2_00000001.png"}, {"segments_info",Json::array({seg})}}})}});
    const std::array<std::pair<std::string,std::string>,1> members{{{"panoptic_object365/objects365_v2_00000001.png",png(1,1,ids)}}};
    tar(input.mask_archive,members);
    if (overflow) { CHECK_THROWS(import_coconut_annotations(input)); return; }
    const auto components = import_coconut_annotations(input);
    REQUIRE(components.size() == 1); REQUIRE(components[0].index.boxes.size() == 1);
    const auto& box = components[0].index.boxes[0];
    CHECK(box.x1 == -1); CHECK(box.x2 == 2); CHECK(box.original_area == 23);
    CHECK((box.flags & kAnnotationMask) != 0); CHECK(box.mask_rle_pairs == 0);
}
TEST_CASE("COCONut JSON requires canonical categories and exact validation image joins", "[coconut]") {
    ScopedTempDir root("coconut-semantic-joins");
    const std::array physical{objects(1,CoconutImageNamespace::Objects365V1), objects(2,CoconutImageNamespace::Objects365V1)};
    auto input = request(physical,CoconutEdition::ObjectsValidation);
    input.annotation_json = root.path()/"val.json"; input.mask_archive = root.path()/"val.tar";
    Json annotation{{"image_id",11},{"file_name","11.png"},{"object365_file_name","objects365_v1_00000001"},{"segments_info",Json::array({segment()})}};
    Json document{{"categories",category_catalog()},{"images",Json::array({{{"id",11},{"file_name","11.png"},{"width",1},{"height",1}}})},
        {"annotations",Json::array({annotation})}};
    bool valid = false;
    SECTION("canonical names and record-level thing admission") { valid = true; }
    SECTION("mismatched mapped name") { document["categories"][0]["name"] = "car"; }
    SECTION("missing mapped category") { document["categories"].erase(0); }
    SECTION("duplicate mapped category") { document["categories"].push_back(document["categories"][0]); }
    SECTION("malformed mapped category") { document["categories"][0]["id"] = "1"; }
    SECTION("orphan validation annotation") { document["images"] = Json::array(); }
    SECTION("two physical members join one image row") {
        annotation["object365_file_name"] = "objects365_v1_00000002";
        document["annotations"].push_back(annotation);
    }
    json_file(input.annotation_json,document);
    const std::array<std::uint32_t,1> ids{1};
    const std::array<std::pair<std::string,std::string>,1> members{{{"panoptic_o365val_v3/objects365_v1_00000001.png",png(1,1,ids)}}};
    tar(input.mask_archive,members);
    if (!valid) { CHECK_THROWS(import_coconut_annotations(input)); return; }
    const auto components = import_coconut_annotations(input);
    REQUIRE(components.size()==1); REQUIRE(components[0].index.boxes.size()==1);
    CHECK(components[0].index.boxes[0].class_id==0); CHECK(components[0].index.boxes[0].source_category_id==1);
    CHECK(components[0].inventory[0].release_image_id==11); CHECK(components[0].index.images[0].source_image_id==1);
}
TEST_CASE("COCONut preparatory arrays accept either order and reject duplicate arrays", "[coconut]") {
    ScopedTempDir root("coconut-envelope-order");
    const std::array physical{objects(1,CoconutImageNamespace::Objects365V1)};
    auto input=request(physical,CoconutEdition::ObjectsValidation);
    input.annotation_json=root.path()/"val.json"; input.mask_archive=root.path()/"val.tar";
    const auto categories=category_catalog().dump();
    const auto images=Json::array({{{"id",11},{"file_name","11.png"},{"width",3},{"height",1}}}).dump();
    const auto annotations=Json::array({{{"image_id",11},{"file_name","11.png"},
        {"object365_file_name","objects365_v1_00000001"},
        {"segments_info",Json::array({segment(1,1),segment(2,13),segment(3,90)})}}}).dump();
    std::string document;
    bool valid=true;
    SECTION("categories before images") {
        document="{\"categories\":"+categories+",\"images\":"+images+",\"annotations\":"+annotations+"}";
    }
    SECTION("annotations before images before categories") {
        document="{\"annotations\":"+annotations+",\"images\":"+images+",\"categories\":"+categories+"}";
    }
    SECTION("duplicate categories after images") {
        valid=false;
        document="{\"categories\":"+categories+",\"images\":"+images+",\"categories\":"+categories+",\"annotations\":"+annotations+"}";
    }
    SECTION("duplicate images after categories") {
        valid=false;
        document="{\"images\":"+images+",\"categories\":"+categories+",\"images\":"+images+",\"annotations\":"+annotations+"}";
    }
    mmltk::testsupport::write_text_file(input.annotation_json,document);
    const std::array<std::uint32_t,3> ids{1,2,3};
    const std::array<std::pair<std::string,std::string>,1> members{{{"panoptic_o365val_v3/objects365_v1_00000001.png",png(3,1,ids)}}};
    tar(input.mask_archive,members);
    if (!valid) { CHECK_THROWS(import_coconut_annotations(input)); return; }
    const auto components=import_coconut_annotations(input);
    REQUIRE(components.size()==1); REQUIRE(components[0].index.boxes.size()==3);
    const auto& boxes=components[0].index.boxes;
    CHECK(boxes[0].source_category_id==1); CHECK(boxes[0].class_id==0);
    CHECK(boxes[1].source_category_id==13); CHECK(boxes[1].class_id==11);
    CHECK(boxes[2].source_category_id==90); CHECK(boxes[2].class_id==79);
    CHECK(components[0].inventory[0].release_image_id==11);
    CHECK(components[0].index.images[0].source_image_id==1);
}
TEST_CASE("COCONut compact inventory is deterministic and jointly admitted under corruption and cancellation", "[coconut]") {
    ScopedTempDir root("coconut-compact-inventory");
    const std::array physical{coco(7),coco(8)};
    const std::array<std::uint32_t,1> ids{1};
    auto input=request(physical); input.parquet_shards={root.path()/"base.parquet"};
    parquet_file(input.parquet_shards[0],Json::array({hf_row(8,png(1,1,ids),Json::array({segment()}),1,1),hf_row(7,png(1,1,ids),Json::array({segment()}),1,1)}));
    const auto first=import_coconut_annotations(input);
    const auto rebuilt=import_coconut_annotations(input);
    REQUIRE(first.size()==1); REQUIRE(rebuilt.size()==1);
    CHECK(first[0].index.annotation_sha256==rebuilt[0].index.annotation_sha256);
    REQUIRE(first[0].index.images.size() == 2); REQUIRE(first[0].index.boxes.size() == 2);
    CHECK(first[0].index.images[0].source_image_id == 7); CHECK(first[0].index.images[1].source_image_id == 8);
    CHECK(first[0].index.images[0].first_box == 0); CHECK(first[0].index.images[1].first_box == 1);
    CHECK(first[0].index.boxes[0].source_ordinal == 1); CHECK(first[0].index.boxes[1].source_ordinal == 0);
    CHECK(first[0].inventory[0].release_image_id == 7); CHECK(first[0].inventory[1].release_image_id == 8);

    const auto path=root.path()/"first.bin", second=root.path()/"second.bin";
    PollCancellation baseline;
    store_coconut_component(path,first[0],mmltk::common::concurrency::CancellationObservation::Borrow(baseline));
    store_coconut_component(second,rebuilt[0]);
    const auto original=file_bytes(path.string()+".inventory");
    CHECK(original==file_bytes(second.string()+".inventory"));
    CHECK(file_bytes(path)==file_bytes(second));
    REQUIRE(original.size()>64); CHECK(original.substr(0,8)=="CNUTIVN1");
    const auto loaded=load_coconut_component(path,first[0].edition,first[0].source,input.input_identity);
    REQUIRE(loaded); CHECK(loaded->inventory==first[0].inventory);
    SECTION("truncation") {
        std::filesystem::resize_file(path.string()+".inventory",original.size()-1);
        CHECK_FALSE(load_coconut_component(path,first[0].edition,first[0].source,input.input_identity));
    }
    SECTION("corruption") {
        auto corrupt=original; corrupt[corrupt.size()/2]^=1;
        mmltk::testsupport::write_text_file(path.string()+".inventory",corrupt);
        CHECK_FALSE(load_coconut_component(path,first[0].edition,first[0].source,input.input_identity));
    }
    SECTION("reordered inventory cannot replace an admitted index") {
        auto reordered=first[0]; std::swap(reordered.inventory[0],reordered.inventory[1]);
        CHECK_THROWS(store_coconut_component(path,reordered));
        CHECK(file_bytes(path.string()+".inventory")==original);
    }
    SECTION("every store cancellation point prevents fresh admission") {
        for (std::size_t cut=0; cut<baseline.polls; ++cut) {
            const auto cancelled_path=root.path()/("cancelled-"+std::to_string(cut)+".bin");
            PollCancellation stop; stop.stop_at=cut;
            CHECK_THROWS(store_coconut_component(cancelled_path,first[0],mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
            CHECK_FALSE(load_coconut_component(cancelled_path,first[0].edition,first[0].source,input.input_identity));
        }
    }
    SECTION("load cancellation propagates instead of becoming a cache miss") {
        PollCancellation observed;
        REQUIRE(load_coconut_component(path,first[0].edition,first[0].source,input.input_identity,mmltk::common::concurrency::CancellationObservation::Borrow(observed)));
        for (std::size_t cut=0; cut<observed.polls; ++cut) {
            PollCancellation stop; stop.stop_at=cut;
            CHECK_THROWS(load_coconut_component(path,first[0].edition,first[0].source,input.input_identity,mmltk::common::concurrency::CancellationObservation::Borrow(stop)));
        }
    }
}
TEST_CASE("COCONut cancellation after the last record covers consolidation and identity encoding", "[coconut]") {
    ScopedTempDir root("coconut-finalization-cancel");
    const std::array physical{coco(7),coco(8)};
    const std::array<std::uint32_t,1> ids{1};
    auto input=request(physical); input.parquet_shards={root.path()/"base.parquet"};
    parquet_file(input.parquet_shards[0],Json::array({hf_row(8,png(1,1,ids),Json::array({segment()}),1,1),hf_row(7,png(1,1,ids),Json::array({segment()}),1,1)}));
    struct FinalRecordCancellation {
        mutable PollCancellation counter;
        bool armed=false;
        bool cancelled() const noexcept { return armed && counter.cancelled(); }
    } state;
    input.cancellation=mmltk::common::concurrency::CancellationObservation::Borrow(state);
    input.progress=[&](std::uint64_t count) { if (count==2) state.armed=true; };
    REQUIRE(import_coconut_annotations(input).size()==1);
    const auto checks=state.counter.polls;
    REQUIRE(checks>1);
    for (std::size_t cut=0; cut<checks; ++cut) {
        state.armed=false; state.counter.polls=0; state.counter.stop_at=cut;
        CHECK_THROWS(import_coconut_annotations(input)); CHECK(state.armed);
    }
}
