#include <arrow/api.h>
#include <arrow/memory_pool.h>
#include <parquet/arrow/reader.h>
#include <nlohmann/json.hpp>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using Json = nlohmann::json;
template <class T> T take(arrow::Result<T> result) {
 if (!result.ok()) throw std::runtime_error(result.status().ToString());
 return std::move(result).ValueOrDie();
}
template <class T> std::shared_ptr<T> as(const std::shared_ptr<arrow::Array>& value) {
 auto result = std::dynamic_pointer_cast<T>(value);
 if (!result) throw std::runtime_error("missing or incompatible COCONut field");
 return result;
}
Json fields(const arrow::StructArray& array, std::int64_t row) {
 Json result = Json::object();
 for (int i = 0; i < array.num_fields(); ++i) {
  const auto& field = array.field(i);
  const auto name = array.struct_type()->field(i)->name();
  if (field->IsNull(row)) result[name] = nullptr;
  else if (field->type_id() == arrow::Type::STRING) result[name] = take(field->GetScalar(row))->ToString();
  else result[name] = Json::parse(take(field->GetScalar(row))->ToString());
 }
 return result;
}
void inspect(std::string_view encoded, std::uint64_t id, Json metadata, bool export_image) {
 if (encoded.size() > 64U * 1024U * 1024U) throw std::runtime_error("mask exceeds 64 MiB");
 int width = 0, height = 0, channels = 0;
 const auto* bytes = reinterpret_cast<const unsigned char*>(encoded.data());
 if (!stbi_info_from_memory(bytes, static_cast<int>(encoded.size()), &width, &height, &channels) || width <= 0 || height <= 0 ||
     width > 32767 || height > 32767 || static_cast<std::uint64_t>(width) * height > 64U * 1024U * 1024U)
  throw std::runtime_error("invalid or oversized mask geometry");
 std::unique_ptr<unsigned char, decltype(&stbi_image_free)> pixels(
  stbi_load_from_memory(bytes, static_cast<int>(encoded.size()), &width, &height, &channels, 3), stbi_image_free);
 if (!pixels) throw std::runtime_error("cannot decode mask");
 std::map<std::uint32_t, std::uint64_t> counts;
 const auto count = static_cast<std::size_t>(width) * height;
 for (std::size_t i = 0; i < count; ++i) {
  auto* pixel = pixels.get() + i * 3U;
  const auto segment = pixel[0] | (static_cast<std::uint32_t>(pixel[1]) << 8U) | (static_cast<std::uint32_t>(pixel[2]) << 16U);
  ++counts[segment];
  if (counts.size() > 65535U) throw std::runtime_error("mask has too many segment IDs");
  if (export_image) {
   const auto color = segment * 2654435761U;
   pixel[0] = segment ? 64U + (color & 127U) : 0;
   pixel[1] = segment ? 64U + ((color >> 8U) & 127U) : 0;
   pixel[2] = segment ? 64U + ((color >> 16U) & 127U) : 0;
  }
 }
 metadata["kind"] = "panoptic_mask";
 metadata["image_id"] = id;
 metadata["width"] = width;
 metadata["height"] = height;
 metadata["support"] = Json::array();
 for (const auto& [segment, area] : counts) metadata["support"].push_back({{"id", segment}, {"pixels", area}});
 if (metadata.contains("segments"))
  for (auto& segment : metadata["segments"]) segment["observed_pixels"] = counts[segment.at("id").get<std::uint32_t>()];
 if (export_image) {
  const auto prefix = "/evidence/" + std::to_string(id);
  std::ofstream raw(prefix + ".mask.png", std::ios::binary);
  raw.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
  raw.close();
  if (!raw || !stbi_write_png((prefix + ".segments.png").c_str(), width, height, 3, pixels.get(), width * 3))
   throw std::runtime_error("cannot export mask evidence");
  metadata["export_prefix"] = "build/validation/benchmark-image/" + std::to_string(id);
 }
 std::cout << metadata.dump() << '\n';
}
int main(int argc, char** argv) try {
 if (argc != 5) throw std::runtime_error("expected mode, file, image ID, export flag");
 const std::string mode = argv[1], path = argv[2];
 const auto id = std::stoull(argv[3]);
 const bool export_image = std::string_view(argv[4]) == "1";
 if (mode == "png") {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream || stream.tellg() < 0 || stream.tellg() > 64 * 1024 * 1024) throw std::runtime_error("invalid mask file");
  std::string bytes(static_cast<std::size_t>(stream.tellg()), '\0');
  stream.seekg(0);
  if (!stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) throw std::runtime_error("cannot read mask file");
  inspect(bytes, id, {{"path", path}}, export_image);
  return 0;
 }
 if (mode != "parquet") throw std::runtime_error("unknown mask diagnostic mode");
 arrow::ProxyMemoryPool tracked(arrow::system_memory_pool());
 arrow::CappedMemoryPool pool(&tracked, 256LL * 1024LL * 1024LL);
 parquet::ReaderProperties input(&pool);
 input.enable_buffered_stream();
 input.set_buffer_size(128U * 1024U);
 parquet::ArrowReaderProperties properties;
 properties.set_pre_buffer(false);
 properties.set_use_threads(false);
 properties.set_batch_size(8);
 parquet::arrow::FileReaderBuilder builder;
 const auto opened = builder.OpenFile(path, false, input);
 if (!opened.ok()) throw std::runtime_error(opened.ToString());
 builder.memory_pool(&pool);
 builder.properties(properties);
 auto reader = take(builder.Build());
 auto batches = take(reader->GetRecordBatchReader());
 std::uint64_t ordinal = 0;
 while (auto batch = take(batches->Next())) {
  const auto valid = batch->ValidateFull();
  if (!valid.ok()) throw std::runtime_error(valid.ToString());
  const auto images = as<arrow::StructArray>(batch->GetColumnByName("image_info"));
  const auto ids = as<arrow::Int64Array>(images->GetFieldByName("id"));
  for (std::int64_t row = 0; row < batch->num_rows(); ++row, ++ordinal) {
   if (images->IsNull(row) || ids->IsNull(row)) continue;
   if (static_cast<std::uint64_t>(ids->Value(row)) != id) continue;
   const auto masks = as<arrow::StructArray>(batch->GetColumnByName("mask"));
   const auto bytes = as<arrow::BinaryArray>(masks->GetFieldByName("bytes"));
   const auto annotations = as<arrow::StructArray>(batch->GetColumnByName("segments_info"));
   const auto list = as<arrow::ListArray>(annotations->GetFieldByName("segments_info"));
   const auto segments = as<arrow::StructArray>(list->values());
   if (masks->IsNull(row) || bytes->IsNull(row) || annotations->IsNull(row) || list->IsNull(row) || list->value_length(row) > 65535)
    throw std::runtime_error("missing or oversized COCONut mask record");
   Json metadata{{"path", path}, {"row", ordinal}, {"image_info", fields(*images, row)}, {"segments", Json::array()}};
   const auto begin = list->value_offset(row);
   for (std::int64_t i = 0; i < list->value_length(row); ++i) metadata["segments"].push_back(fields(*segments, begin + i));
   inspect(bytes->GetView(row), id, std::move(metadata), export_image);
   return 0;
  }
 }
 throw std::runtime_error("image ID is absent from this Parquet shard");
} catch (const std::exception& error) {
 std::cerr << "COCONut mask diagnostic: " << error.what() << '\n';
 return 1;
}
