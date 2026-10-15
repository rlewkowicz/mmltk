#include "src/backend/data/benchmark/detail/benchmark_download.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/data/benchmark/detail/benchmark_trace.h"
#include "src/backend/data/tests/test_fixture.h"
#include "src/backend/data/benchmark/detail/benchmark_writer.h"
#include "src/backend/data/benchmark/detail/benchmark_labels.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_progress.h"
#include "src/backend/data/detail/compile_pixel_workspace.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_import.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_native_image.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_parquet.h"
#include "src/backend/data/benchmark/coconut/detail/coconut_physical.h"
#include "src/common/io/file_digest.h"
#include "src/backend/data/detail/compile_image_decoder.h"
#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
#include "src/backend/data/detail/file_work_queue.h"
#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/common/math/checked_arithmetic.h"
#include <map>
#include "src/backend/data/compiler/dataset_compiler.h"
#include <stb_image_write.h>
#include <archive.h>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>
#include <archive_entry.h>
#include <array>
#include <atomic>
#include <concepts>
#include <type_traits>
#include <variant>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
namespace fs = std::filesystem;
namespace mmltk::backend::data::testsupport {
using Json = nlohmann::json;
void check_normal_compile_storage(const detail::CompileExecution& execution, std::uint64_t locked_bytes) {
 CHECK(execution.snapshot().exceptional == 0);
 CHECK(execution.snapshot().normal_locked == locked_bytes);
}
void check_compile_records_drained(const detail::CompileExecution& execution) {
 CHECK(execution.snapshot().records == 0);
 CHECK(execution.snapshot().leased == 0);
}
void check_compile_inputs_drained(const detail::CompileExecution& execution) {
 CHECK(execution.snapshot().leased == 0);
 CHECK(execution.resource_usage().descriptors == 0);
}
void set_private_archive_entry(archive_entry* entry, std::size_t bytes) {
 archive_entry_set_filetype(entry, AE_IFREG);
 archive_entry_set_perm(entry, 0600);
 archive_entry_set_size(entry, bytes);
}
void arrow_ok(const arrow::Status& status) {
 if (!status.ok()) throw std::runtime_error(status.ToString());
}
std::shared_ptr<arrow::DataType> coconut_parquet_segment_type(bool integer_area) {
 return arrow::struct_(
  {arrow::field("area", integer_area ? arrow::int64() : arrow::float64()), arrow::field("category_id", arrow::int64()), arrow::field("id", arrow::int64()), arrow::field("iscrowd", arrow::int64()),
   arrow::field("isthing", arrow::int64())});
}
std::shared_ptr<arrow::Schema> coconut_parquet_schema(bool integer_area) {
 return arrow::schema(
  {arrow::field("mask", arrow::struct_({arrow::field("bytes", arrow::binary()), arrow::field("path", arrow::utf8())})),
   arrow::field("segments_info",
    arrow::struct_({arrow::field("file_name", arrow::utf8()), arrow::field("image_id", arrow::int64()), arrow::field("segments_info", arrow::list(coconut_parquet_segment_type(integer_area)))})),
   arrow::field("image_info", arrow::struct_(
                               {arrow::field("coco_url", arrow::utf8()), arrow::field("date_captured", arrow::utf8()), arrow::field("file_name", arrow::utf8()), arrow::field("height", arrow::int64()),
                                arrow::field("id", arrow::int64()), arrow::field("license", arrow::int64()), arrow::field("width", arrow::int64())}))});
}
namespace {
void append_value(arrow::ArrayBuilder& builder, const Json& value) {
 if (value.is_null()) {
  arrow_ok(builder.AppendNull());
  return;
 }
 switch (builder.type()->id()) {
  case arrow::Type::STRUCT: {
   auto& typed = static_cast<arrow::StructBuilder&>(builder);
   arrow_ok(typed.Append());
   const auto type = std::static_pointer_cast<const arrow::StructType>(builder.type());
   for (int i = 0; i < type->num_fields(); ++i) append_value(*typed.field_builder(i), value.at(type->field(i)->name()));
   break;
  }
  case arrow::Type::LIST: {
   auto& typed = static_cast<arrow::ListBuilder&>(builder);
   arrow_ok(typed.Append());
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
}  // namespace
void write_coconut_parquet(const std::filesystem::path& path, const Json& rows, std::shared_ptr<arrow::Schema> schema, std::int64_t group_rows,
 std::shared_ptr<parquet::ArrowWriterProperties> arrow_properties, std::shared_ptr<parquet::WriterProperties> writer_properties) {
 if (!schema) schema = coconut_parquet_schema();
 if (!arrow_properties) arrow_properties = parquet::default_arrow_writer_properties();
 std::vector<std::shared_ptr<arrow::Array>> columns;
 for (const auto& field : schema->fields()) {
  std::unique_ptr<arrow::ArrayBuilder> builder;
  arrow_ok(arrow::MakeBuilder(arrow::default_memory_pool(), field->type(), &builder));
  for (const auto& row : rows) append_value(*builder, row.at(field->name()));
  std::shared_ptr<arrow::Array> column;
  arrow_ok(builder->Finish(&column));
  columns.push_back(std::move(column));
 }
 auto opened = arrow::io::FileOutputStream::Open(path.string());
 arrow_ok(opened.status());
 auto output = std::move(opened).ValueOrDie();
 parquet::WriterProperties::Builder properties;
 properties.compression(parquet::Compression::SNAPPY);
 arrow_ok(parquet::arrow::WriteTable(
  *arrow::Table::Make(schema, columns), arrow::default_memory_pool(), output, group_rows, writer_properties ? std::move(writer_properties) : properties.build(), std::move(arrow_properties)));
 arrow_ok(output->Close());
}
ProfileImageFixture profile_image_fixture(std::uint32_t width, std::uint32_t height, FixtureImageEncoding encoding, std::uint32_t seed, std::size_t byte_limit) {
 if (!width || !height || width > MAX_IMAGE_EXTENT || height > MAX_IMAGE_EXTENT) throw std::invalid_argument("profile fixture geometry is invalid");
 const auto pixels = mmltk::common::math::checked_multiply<std::size_t>(width, height, "profile synthetic extent overflow");
 const auto bytes = mmltk::common::math::checked_multiply(pixels, std::size_t{3}, "profile synthetic RGB extent overflow");
 if (bytes > byte_limit) throw std::length_error("profile synthetic image exceeds input cap before allocation");
 ProfileImageFixture result{width, height, {}};
 std::vector<std::uint8_t> rgb(bytes);
 for (std::size_t i = 0; i < rgb.size(); ++i) {
  seed ^= seed << 13;
  seed ^= seed >> 17;
  seed ^= seed << 5;
  rgb[i] = static_cast<std::uint8_t>(seed);
 }
 struct Encoding {
  std::vector<std::uint8_t>& bytes;
  std::size_t limit;
  bool overflow = false;
 } output{result.encoded, byte_limit};
 const auto append = +[](void* context, void* data, int count) {
  auto& destination = *static_cast<Encoding*>(context);
  if (destination.overflow || count < 0 || static_cast<std::size_t>(count) > destination.limit - destination.bytes.size()) {
   destination.overflow = true;
   return;
  }
  const auto* begin = static_cast<std::uint8_t*>(data);
  destination.bytes.insert(destination.bytes.end(), begin, begin + count);
 };
 const auto written = encoding == FixtureImageEncoding::Jpeg ? stbi_write_jpg_to_func(append, &output, static_cast<int>(width), static_cast<int>(height), 3, rgb.data(), 95)
                                                             : stbi_write_png_to_func(append, &output, static_cast<int>(width), static_cast<int>(height), 3, rgb.data(), static_cast<int>(width * 3));
 if (output.overflow) throw std::length_error("profile encoded image exceeds input cap");
 if (!written) throw std::runtime_error("cannot encode profile fixture");
 return result;
}
std::vector<std::string> profile_archive_fixture(const fs::path& path, std::span<const ProfileImageFixture> inputs, std::span<const std::size_t> order, std::uint64_t byte_limit) {
 if (order.empty() || order.size() > 256 || byte_limit > (64U << 20)) throw std::length_error("archive fixture exceeds bounded source policy");
 std::uint64_t bytes = 1024;
 for (auto index : order) {
  if (index >= inputs.size()) throw std::out_of_range("archive fixture input index");
  bytes = mmltk::common::math::checked_add(bytes, static_cast<std::uint64_t>(inputs[index].encoded.size()) + 2048, "archive fixture extent overflow");
  if (bytes > byte_limit) throw std::length_error("archive fixture exceeds source byte cap");
 }
 std::unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
 if (!writer) throw std::bad_alloc{};
 const auto require = [&](bool ok) {
  if (!ok) throw std::runtime_error(std::string("cannot create archive fixture: ") + (archive_error_string(writer.get()) ? archive_error_string(writer.get()) : "archive write failure"));
 };
 require(archive_write_set_format_pax(writer.get()) == ARCHIVE_OK);
 require(archive_write_add_filter_gzip(writer.get()) == ARCHIVE_OK);
 // Identical fixture bytes must produce identical causal-profile source hashes.
 require(archive_write_set_filter_option(writer.get(), "gzip", "timestamp", nullptr) == ARCHIVE_OK);
 require(archive_write_open_filename(writer.get(), path.c_str()) == ARCHIVE_OK);
 std::vector<std::string> names;
 names.reserve(order.size());
 for (std::size_t i = 0; i < order.size(); ++i) {
  names.push_back((i ? "images/" : std::string(140, 'p') + "/") + std::to_string(i + 1) + ".jpg");
  const auto& input = inputs[order[i]].encoded;
  std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
  if (!entry) throw std::bad_alloc{};
  archive_entry_set_pathname(entry.get(), names.back().c_str());
  mmltk::backend::data::testsupport::set_private_archive_entry(entry.get(), input.size());
  require(archive_write_header(writer.get(), entry.get()) == ARCHIVE_OK);
  require(archive_write_data(writer.get(), input.data(), input.size()) == static_cast<la_ssize_t>(input.size()));
 }
 require(archive_write_close(writer.get()) == ARCHIVE_OK);
 if (fs::file_size(path) > byte_limit) throw std::length_error("compressed archive fixture exceeds source byte cap");
 return names;
}
std::vector<std::vector<std::uint8_t>> retained_profile_rgb(std::span<const ProfileImageFixture> inputs, std::size_t retained_limit, std::size_t workspace_limit) {
 using namespace detail;
 using namespace mmltk::common::system;
 if (inputs.empty() || inputs.size() > 64) throw std::length_error("profile requires 1-64 retained images");
 CompileImageDecoder decoder;
 std::vector<CompileImageHeader> headers;
 headers.reserve(inputs.size());
 std::size_t retained = 0, supplied = 0, opaque = 0;
 for (const auto& input : inputs) {
  const auto header = decoder.read_header(input.encoded, CompileImagePolicy::Benchmark, input.width, input.height);
  const auto rgb_bytes = header.rgb_bytes();
  if (rgb_bytes > retained_limit - retained) throw std::length_error("profile decoded inputs exceed retained RGB cap");
  retained += rgb_bytes;
  const auto plan = CompileImageDecoder::plan(header);
  const auto bytes = plan.bytes();
  if (bytes > workspace_limit) throw std::length_error("profile decode exceeds bounded workspace budget");
  supplied = std::max(supplied, page_rounded_bytes(bytes));
  opaque = std::max(opaque, plan.opaque_bytes);
  if (opaque > workspace_limit || supplied > workspace_limit - opaque) throw std::length_error("profile decode exceeds bounded workspace budget");
  headers.push_back(header);
 }
 // The complete inventory is admitted. One reusable arena covers all codecs;
 // STB's returned RGB remains within it, while JPEG has a supplied RGB span.
 NumaMemory workspace(NumaTopology::Capture().permitted_nodes.front());
 workspace.ensure_bytes(supplied);
 std::vector<std::vector<std::uint8_t>> decoded;
 decoded.reserve(inputs.size());
 for (std::size_t i = 0; i < inputs.size(); ++i) {
  const auto rgb = decoder.decode_rgb(inputs[i].encoded, headers[i], {static_cast<std::byte*>(workspace.data()), supplied});
  if (rgb.size() != headers[i].rgb_bytes()) throw CompileImageError("profile decoded RGB extent differs from its admitted header");
  decoded.emplace_back(rgb.begin(), rgb.end());
 }
 return decoded;
}
benchmark_internal::NormalizedAnnotationReadView pipeline_annotation_fixture(std::span<const ProfileImageFixture> inputs, std::span<const std::size_t> order) {
 using namespace benchmark_internal;
 if (order.empty() || order.size() > 1024) throw std::length_error("pipeline fixture row count exceeds 1024");
 NormalizedAnnotationBuilder builder;
 builder.split = "train";
 builder.images.reserve(order.size());
 builder.boxes.reserve(order.size());
 for (std::size_t i = 0; i < order.size(); ++i) {
  if (order[i] >= inputs.size()) throw std::out_of_range("pipeline fixture input index");
  const auto& input = inputs[order[i]];
  const auto count = i % 4 ? 1U : 0U;
  builder.images.push_back({i + 1, builder.boxes.size(), count, input.width, input.height, 0, 0});
  if (count) {
   NormalizedBox box;
   box.x1 = box.y1 = 0.25F;
   box.x2 = box.y2 = 0.75F;
   box.annotation_id = i + 1;
   box.source_category_id = 1;
   box.source_ordinal = i;
   box.original_area = static_cast<double>(input.width) * input.height / 4;
   box.flags = kAnnotationId | kAnnotationCategory;
   builder.boxes.push_back(box);
  }
 }
 return NormalizedAnnotationReadView(seal_normalized_annotations(std::move(builder)));
}
namespace {
// Fixture values follow the native external-row vocabulary. Only the binary
// representation is adapted to the shared writer's external JSON input.
template <class T>
Json parquet_fixture_value(const T& value) {
 if constexpr (std::is_arithmetic_v<T> || std::same_as<T, std::string>)
  return value;
 else if constexpr (std::same_as<T, std::vector<std::uint8_t>>)
  return std::string(value.begin(), value.end());
 else if constexpr (requires { value.has_value(); })
  return value ? parquet_fixture_value(*value) : Json{};
 else if constexpr (requires { std::variant_size<T>::value; })
  return std::visit([](const auto& item) { return parquet_fixture_value(item); }, value);
 else if constexpr (requires {
                     value.begin();
                     value.end();
                    }) {
  auto result = Json::array();
  for (const auto& item : value) result.push_back(parquet_fixture_value(item));
  return result;
 } else {
  auto result = Json::object();
  mmltk::frameworks::reflection::visit_materialized_members<T>([&]<class Declaration>(const auto& field) { result[field.member_name] = parquet_fixture_value(value.*Declaration::pointer); });
  return result;
 }
}
}  // namespace
struct PipelineNativeFixture::Impl {
 std::filesystem::path path, staging;
 std::string identity;
 // Membership borrows these immutable entries until its own retirement.
 std::vector<benchmark_internal::CoconutPhysicalImage> identities;
 std::unique_ptr<benchmark_internal::CoconutPhysicalMembership> physical;
 std::shared_ptr<const benchmark_internal::CoconutRecoveryOriginals> originals;
 std::uint64_t workspace = 0;
};
PipelineNativeFixture::PipelineNativeFixture(const std::filesystem::path& directory, std::span<const ProfileImageFixture> inputs, std::span<const std::size_t> order, bool dropped)
    : impl_(std::make_unique<Impl>()) {
 using namespace benchmark_internal;
 if (order.empty() || order.size() > 1024) throw std::length_error("native profile exceeds 1024 rows");
 for (const auto position : order)
  if (position >= inputs.size()) throw std::out_of_range("native fixture input index");
 fs::create_directories(directory);
 impl_->path = directory / "native.parquet";
 impl_->staging = directory / "native-staging";
 std::vector<std::array<std::vector<std::uint8_t>, 2>> masks(inputs.size());
 std::size_t mask_bytes = 0;
 for (std::size_t i = 0; i < inputs.size(); ++i) {
  const auto& input = inputs[i];
  const auto pixels = mmltk::common::math::checked_multiply<std::size_t>(input.width, input.height, "native fixture extent overflow");
  if (pixels > (64U << 20) / 3) throw std::length_error("native fixture canvas exceeds 64 MiB");
  std::vector<std::uint8_t> rgb(pixels * 3);
  for (unsigned foreground = 0; foreground != 2; ++foreground) {
   if (foreground)
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) rgb[pixel * 3] = 1;
   auto& encoded = masks[i][foreground];
   if (!stbi_write_png_to_func([](void* output, void* data, int count) {
    auto& bytes = *static_cast<std::vector<std::uint8_t>*>(output);
    const auto* first = static_cast<const std::uint8_t*>(data);
    bytes.insert(bytes.end(), first, first + count);
   }, &encoded, input.width, input.height, 3, rgb.data(), input.width * 3))
    throw std::runtime_error("cannot encode native fixture mask");
   mask_bytes += encoded.size();
   if (mask_bytes > (64U << 20)) throw std::length_error("native fixture masks exceed 64 MiB");
  }
 }
 NormalizedAnnotationBuilder original;
 original.split = "train2017";
 mmltk::common::io::Sha256Hasher original_identity;
 impl_->identities.reserve(order.size());
 auto rows = Json::array();
 std::uint64_t encoded_bytes = 0;
 for (std::size_t i = 0; i < order.size(); ++i) {
  const auto& input = inputs[order[i]];
  original_identity.Update(input.encoded);
  const auto id = i + 1;
  const auto digits = std::to_string(id);
  const auto stem = std::string(12 - digits.size(), '0') + digits;
  const bool foreground = i % 4 != 0;
  CoconutParquetRow row;
  row.mask.bytes = masks.at(order[i])[foreground && !dropped ? 1 : 0];
  encoded_bytes += row.mask.bytes.size();
  if (encoded_bytes > (64U << 20)) throw std::length_error("native fixture retained rows exceed 64 MiB");
  row.image_info = {stem + ".jpg", "http://images.cocodataset.org/train2017/" + stem + ".jpg", "2017", static_cast<std::int64_t>(id), input.height, input.width, 1};
  row.segments_info.file_name = stem + ".png";
  row.segments_info.image_id = id;
  if (foreground) row.segments_info.segments_info.push_back({.id = 1, .category_id = 1, .isthing = 1, .iscrowd = 0, .ignore = 0, .area = double(input.width) * input.height});
  rows.push_back(parquet_fixture_value(row));
  impl_->identities.push_back({CoconutImageNamespace::CocoTrain, id, 0, "train2017/" + stem + ".jpg", "bounded-native-profile-physical"});
  original.images.push_back({id, original.boxes.size(), foreground ? 1U : 0U, input.width, input.height, 0, 0});
  if (foreground) {
   NormalizedBox box;
   box.x2 = box.y2 = 1;
   box.mask_rle_offset = original.mask_rle_pairs.size();
   box.mask_rle_pairs = 1;
   box.flags = kAnnotationMask | kAnnotationId | kAnnotationCategory;
   box.annotation_id = id + 10000;
   box.source_category_id = 1;
   box.source_ordinal = i;
   box.original_area = double(input.width) * input.height;
   original.boxes.push_back(box);
   original.mask_rle_pairs.push_back({0, input.width * input.height});
  }
 }
 original.annotation_sha256 = mmltk::common::io::sha256_hex(original_identity.Finish());
 const auto index = seal_normalized_annotations(std::move(original));
 impl_->originals = std::make_shared<CoconutRecoveryOriginals>(&index, nullptr);
 impl_->physical = std::make_unique<CoconutPhysicalMembership>(impl_->identities);
 CoconutNativeWorkspace workspace(CoconutImportLimits{});
 workspace.originals(impl_->originals);
 for (std::size_t i = 0; i < order.size(); ++i) {
  const auto& input = inputs[order[i]];
  CoconutRecord record;
  record.image_id = i + 1;
  record.width = input.width;
  record.height = input.height;
  if (i % 4) record.segments = {{.id = 1, .category_id = 1, .isthing = true}};
  const auto support = workspace.support_workspace_bytes(record, std::uint64_t{input.width} * input.height, record.segments.size());
  const auto decode = workspace.decoder_workspace_bytes(masks[order[i]][i % 4 && !dropped ? 1 : 0]);
  impl_->workspace = std::max(impl_->workspace, mmltk::common::math::checked_add<std::uint64_t>(support, decode, "native fixture workspace overflow"));
 }
 write_coconut_parquet(impl_->path, rows, {}, 64);
 impl_->identity = mmltk::common::io::sha256_hex(mmltk::common::io::sha256_file(impl_->path));
}
PipelineNativeFixture::~PipelineNativeFixture() = default;
std::uint64_t PipelineNativeFixture::workspace_bytes() const { return impl_->workspace; }
const std::string& PipelineNativeFixture::input_identity() const { return impl_->identity; }
benchmark_internal::NormalizedAnnotationReadView PipelineNativeFixture::run(benchmark_internal::BenchmarkCompilePipeline& pipeline, const benchmark_internal::BenchmarkTraceSink& trace) {
 using namespace benchmark_internal;
 CoconutImportRequest request;
 request.execution = &pipeline;
 request.input_identity = impl_->identity;
 request.parquet_shards = {impl_->path};
 request.inventory_directory = impl_->staging;
 request.physical_membership = impl_->physical.get();
 request.trace = trace;
 std::atomic<bool> ready{false};
 detail::CompileExecution::CompletionSource originals;
 request.originals = [&](CoconutImageNamespace, bool, std::stop_token) {
  const bool available = ready.load();
  return CoconutOriginalReadiness{available ? impl_->originals : nullptr, available, 1, originals.completion()};
 };
 request.input_finished = [&] {
  ready = true;
  originals.settle();
 };
 auto components = import_coconut_annotations(request);
 if (components.size() != 1) throw std::runtime_error("native fixture did not produce one complete component");
 return components.front().index();
}
benchmark_internal::BenchmarkTraceSink benchmark_trace() {
 if (!std::getenv("MMLTK_TEST_BENCHMARK_TRACE")) return {};
 return [](std::string_view event, const nlohmann::json& fields) { std::fprintf(stderr, "%s\n", nlohmann::json{{"event", event}, {"payload", fields}}.dump().c_str()); };
}
benchmark_internal::IndexingProgressTotals& mixed_coconut_row_progress(benchmark_internal::ProgressReporter& reporter) {
 const std::array<std::uint64_t, 3> extents{241602, 0, 0};
 auto& rows = *reporter.indexing(extents);
 reporter.label_plans(extents.size());
 for (std::size_t i = 0; i < extents.size(); ++i) reporter.label_plan_started(i);
 reporter.pixels(16177, 0, false);
 rows.update(0, 241596, reporter);
 reporter.flush();
 return rows;
}
detail::CompileExecution::Task prepare_benchmark_pixel(benchmark_internal::BenchmarkCompilePipeline& pipeline, benchmark_internal::BenchmarkSplitWriter& writer, std::uint32_t slot) {
 using namespace detail;
 auto& execution = pipeline.execution();
 std::shared_ptr<benchmark_internal::BenchmarkPixelInput> input;
 execution.run(CompileTaskKind::Header, CompileResources::handles(1, false, 1), [&](std::size_t lane) { input = writer.prepare_pixel(slot, lane, {}, execution.current_allowance()); });
 if (!input) throw std::logic_error("cooperative pixel fixture was already completed");
 pipeline.files().submit({.work = [&] { writer.prepare_pixels(slot); }}).wait();
 auto scratch = std::make_shared<CompilePixelWorkspace>();
 return {.kind = CompileTaskKind::Pixels, .resources = writer.pixel_resources(*input), .work = [&writer, slot, input, scratch](std::size_t lane, const CompileAllowance& allowance) {
  writer.write_pixel(slot, lane, input, allowance, *scratch);
 }, .parent = writer.pixel_input_allowance(*input), .finite_consumer = true, .retain_workspace = false};
}
mmltk::common::io::FileHandle create_compiled_fixture_file(const std::filesystem::path& path, const FileHeader& header) {
 (void)validate_compiled_file_sections(header, header.total_file_size);
 auto file = mmltk::common::io::FileHandle::create_output(path.string(), 0);
 file.resize_sparse(header.total_file_size);
 file.pwrite_all(&header, sizeof(header), 0);
 return file;
}
std::string benchmark_semantics(const std::filesystem::path& path) {
 const auto dataset = CompiledDataset::open(path);
 std::map<std::pair<AnnotationSource, std::uint64_t>, std::string> samples;
 const auto pixel_bytes = std::size_t{dataset.header().image_width} * dataset.header().image_height * 3 * sizeof(float);
 for (const auto slot : dataset.eligible_slots()) {
  const auto& entry = dataset.image_entry(slot);
  std::string value(reinterpret_cast<const char*>(&entry), sizeof(entry));
  const auto payload = dataset.label_payload(slot);
  value.append(reinterpret_cast<const char*>(payload.data()), payload.size());
  value.append(reinterpret_cast<const char*>(dataset.image_pixels(slot)), pixel_bytes);
  if (!samples.emplace(std::pair{entry.source, entry.source_image_id}, std::move(value)).second) throw std::logic_error("duplicate compiled sample identity");
 }
 std::string result;
 for (const auto& name : dataset.class_names()) {
  result += name;
  result.push_back('\0');
 }
 for (const auto& [identity, sample] : samples) {
  (void)identity;
  result += sample;
 }
 return result;
}
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
 "person",
 "ret",
 "scope",
 "iron_sight",
 "anchor_dot",
 "glint",
};
void write_synthetic_sample(const fs::path& split_dir, const int image_index, const int width, const int height, const int background_images, const bool pixel_evidence) {
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
 annotations << R"({"class":")" << kClassNames[cls] << R"(","bbox_xyxy":[)" << x1 << "," << y1 << "," << x2 << "," << y2 << R"(],"mask_rle_encoding":"row_major_start_length","mask_rle":")" << rle
             << R"(","image_size_wh":[)" << width << "," << height << R"(]})"
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
std::vector<RLEPair> alternating_mask_runs(std::size_t count) {
 std::vector<RLEPair> runs(count);
 for (std::size_t index = 0; index < runs.size(); ++index) runs[index] = {static_cast<std::uint32_t>(2 * index), 1};
 return runs;
}
void seed_partial_benchmark_download(const benchmark_internal::DownloadRequest& request, std::span<const std::uint8_t> retained) {
 mmltk::testsupport::write_binary_file(request.destination.string() + ".part", retained);
 benchmark_internal::write_json_atomically(
  request.destination.string() + ".part.json", {{"schema_version", benchmark_internal::kBenchmarkCacheSchemaVersion}, {"url", request.url}, {"etag", "\"benchmark-test-etag\""}}, {});
}
void compile_existing_fixture(const FixtureSpec& spec) {
 auto config = compiler_config(spec);
 config.num_workers = 1;
 const auto plan = DatasetCompiler::prepare(config, {config.split});
 DatasetCompiler::compile(plan, 0U);
}
void create_compiled_fixture(const FixtureSpec& spec, const bool keypoints) {
 if (keypoints)
  create_keypoint_dataset(spec);
 else
  create_synthetic_dataset(spec);
 compile_existing_fixture(spec);
}
DatasetLoader compile_fixture_loader(const FixtureSpec& spec) {
 compile_existing_fixture(spec);
 return DatasetLoader({.compiled_path = compiled_bin_path(spec), .batch_size = 1, .shuffle = false});
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
    << spec.first_class_id << R"(,"name":"person"},)" << R"({"id":)" << spec.first_class_id + 1 << R"(,"name":"ret"},)" << R"({"id":)" << spec.first_class_id + 2 << R"(,"name":"scope"},
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
void create_keypoint_dataset(const FixtureSpec& spec) {
 if (spec.num_images < 2 || spec.width < 32 || spec.height < 32) throw std::invalid_argument("keypoint fixture requires two images and a 32-pixel extent");
 create_synthetic_dataset(spec);
 const auto person = catalog::coco_person_keypoint_schema();
 const catalog::ClassKeypointSchema marker{{"left", "right"}, {{0, 1}}, {1, 0}, {.1, .1}};
 Json classes = Json::array();
 const std::array<std::string, 3> names{"person", "marker", "object"};
 const std::array schemas{person, marker, catalog::ClassKeypointSchema{}};
 for (std::size_t index = 0; index < names.size(); ++index) {
  const auto& schema = schemas[index];
  Json edges = Json::array();
  for (const auto& edge : schema.skeleton) edges.push_back({edge.first, edge.second});
  classes.push_back({{"id", index}, {"name", names[index]}, {"keypoints", schema.names}, {"skeleton", edges}, {"horizontal_flip", schema.horizontal_flip}, {"oks_sigmas", schema.oks_sigmas}});
 }
 std::ofstream(fs::path(dataset_dir(spec)) / "categories.json") << Json{{"classes", classes}, {"splits", {{spec.split, {{"total", spec.num_images}}}}}}.dump();
 for (int image = 1; image <= spec.num_images; ++image) {
  char name[32];
  std::snprintf(name, sizeof(name), "%06d.jsonl", image);
  std::ofstream out(fs::path(dataset_dir(spec)) / spec.split / name);
  if (image == 1) continue;
  for (std::size_t index = 0; index < names.size(); ++index) {
   Json joints = Json::array();
   for (std::size_t joint = 0; joint < schemas[index].names.size(); ++joint) {
    const auto visibility = joint % 3 == 0 ? 2 : joint % 3 == 1 ? 1 : 0;
    joints.push_back(visibility ? 4 + joint : 0);
    joints.push_back(visibility ? 6 + joint : 0);
    joints.push_back(visibility);
   }
   out
    << Json{{"class", names[index]}, {"bbox_xyxy", {2, 2, spec.width - 2, spec.height - 2}}, {"keypoints", joints}, {"image_size_wh", {spec.width, spec.height}}, {"id", image * 10 + index}, {"image_id", image}, {"category_id", index + 1}}
        .dump()
    << '\n';
  }
 }
}
void replace_synthetic_image(const FixtureSpec& spec, const int image_index, const int width, const int height) {
 if (image_index < 1 || image_index > spec.num_images || width <= 30 || height <= 30) throw std::invalid_argument("synthetic replacement image is invalid");
 write_synthetic_sample(fs::path(dataset_dir(spec)) / spec.split, image_index, width, height, std::clamp(spec.background_images, 0, spec.num_images), spec.pixel_evidence);
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
std::vector<float> expected_resized_rgb(std::span<const std::uint8_t> rgb, const std::uint32_t width, const std::uint32_t height, const std::uint32_t target_width, const std::uint32_t target_height,
 const mmltk::backend::imaging::resample::ImageResizeMode mode, const bool perceptual) {
 using namespace mmltk::backend::imaging::resample;
 if (rgb.size() != std::size_t(width) * height * 3U) throw std::invalid_argument("RGB fixture extent mismatch");
 const auto geometry = compute_image_resize_geometry(width, height, target_width, target_height, mode);
 RgbImageResizer resizer(1, perceptual);
 // CLEANUP-IGNORE: The standalone resampler test retains its independent projection oracle rather than using the data-compiler fixture being checked.
 std::vector<std::uint8_t> bytes(std::size_t(geometry.resized_width) * geometry.resized_height * 3U);
 resizer.resize(rgb.data(), static_cast<int>(width), static_cast<int>(height), bytes.data(), static_cast<int>(geometry.resized_width), static_cast<int>(geometry.resized_height));
 std::vector<float> expected(std::size_t(target_width) * target_height * 3U);
 letterboxed_rgb_hwc_u8_to_nchw_f32(bytes.data(), expected.data(), geometry.resized_width, geometry.resized_height, target_width, target_height, geometry.offset_x, geometry.offset_y);
 return expected;
}
void assert_image_matches(const float* actual, const std::vector<float>& expected) {
 if (expected.size() <= 8) { throw std::runtime_error("expected image fixture must contain more than eight samples"); }
 for (size_t i = 0; i < expected.size(); ++i) {
  if (std::fabs(actual[i] - expected[i]) >= 1e-6f) { throw std::runtime_error("fixture image mismatch at index " + std::to_string(i)); }
 }
}
}  // namespace mmltk::backend::data::testsupport
namespace mmltk::backend::data::testsupport {
void bind_benchmark_slots(benchmark_internal::BenchmarkCompilePipeline& execution, benchmark_internal::BenchmarkSplitWriter& writer, const benchmark_internal::PreparedBenchmarkSplit& split) {
 for (std::size_t i = 0; i < split.images.size(); ++i) {
  const auto& image = split.images[i];
  execution.bind_slot(split.sources.at(image.source_index).root, image.source_image_id, writer, static_cast<std::uint32_t>(i));
 }
}
ImageEntry benchmark_entry(const benchmark_internal::EncodedImageRecord& image) {
 ImageEntry entry{};
 entry.source_image_id = image.source_image_id;
 entry.has_source_image_id = 1;
 entry.source = image.annotation_source;
 entry.original_width = image.source_width;
 entry.original_height = image.source_height;
 return entry;
}
void write_benchmark_fixture(const benchmark_internal::BenchmarkWriteRequest& request, std::span<const std::shared_ptr<benchmark_internal::BenchmarkLabelChunk>> chunks) {
 benchmark_internal::BenchmarkSplitWriter writer(request);
 for (std::size_t i = 0; i < chunks.size(); ++i) writer.write_labels(static_cast<std::uint32_t>(i), benchmark_entry(request.split.images.at(i)), {chunks[i]->labels, chunks[i]->runs});
 writer.write_remaining(request);
 writer.finish(request);
}
}  // namespace mmltk::backend::data::testsupport
