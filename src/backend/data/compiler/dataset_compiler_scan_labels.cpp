#include "src/backend/data/compiler/detail/dataset_compiler_internal.h"
#include "src/backend/data/compiler/detail/dataset_annotation_json.h"
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/common/math/checked_arithmetic.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/spdlog.h>
#include <stdexcept>
#include <string_view>
#include <utility>
import mmltk.common.logging.mmltk_logging;
import mmltk.common.logging.profile_utils;
namespace mmltk::backend::data::compiler_internal {
using mmltk::common::math::checked_add;
using mmltk::common::math::checked_cast;
using mmltk::common::math::checked_multiply;
using json = nlohmann::json;
namespace {
std::filesystem::path numbered_path(const std::filesystem::path& dir, uint32_t one_based_index, const char* extension) { return dir / std::format("{:06}{}", one_based_index, extension); }
uint32_t count_sequential_images(const std::filesystem::path& split_dir, mmltk::common::concurrency::CancellationObservation cancel_requested) {
 mmltk::common::logging::ScopedProfile profile{"compiler.scan.count_sequential_images"};
 if (!std::filesystem::exists(split_dir) || !std::filesystem::is_directory(split_dir)) { throw std::runtime_error("split directory not found: " + split_dir.string()); }
 uint32_t png_count = 0U;
 uint32_t jsonl_count = 0U;
 uint32_t max_png_index = 0U;
 uint32_t max_jsonl_index = 0U;
 for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(split_dir)) {
  if (cancel_requested.requested()) { throw std::runtime_error("dataset compilation cancelled"); }
  const std::string extension = entry.path().extension().string();
  if (extension != ".png" && extension != ".jsonl") { continue; }
  const std::string stem = entry.path().stem().string();
  uint32_t one_based_index = 0U;
  const auto parsed = std::from_chars(stem.data(), stem.data() + stem.size(), one_based_index);
  if (stem.size() != 6U || parsed.ec != std::errc{} || parsed.ptr != stem.data() + stem.size() || one_based_index == 0U) {
   throw std::runtime_error("dataset split contains a non-sequential annotation asset: " + entry.path().string());
  }
  if (extension == ".png") {
   ++png_count;
   max_png_index = std::max(max_png_index, one_based_index);
  } else {
   ++jsonl_count;
   max_jsonl_index = std::max(max_jsonl_index, one_based_index);
  }
 }
 if (png_count == 0U) { throw std::runtime_error("no sequential PNG images found under: " + split_dir.string()); }
 if (png_count != max_png_index || jsonl_count != max_jsonl_index || png_count != jsonl_count) {
  throw std::runtime_error("dataset split PNG and JSONL assets must form matching contiguous sequences: " + split_dir.string());
 }
 mmltk::common::logging::profile_set_value("compiler.scan.num_images", png_count);
 return png_count;
}
}
std::filesystem::path image_path(const std::filesystem::path& split_dir, uint32_t zero_based_index) { return numbered_path(split_dir, zero_based_index + 1, ".png"); }
std::filesystem::path annotation_path(const std::filesystem::path& split_dir, uint32_t zero_based_index) { return numbered_path(split_dir, zero_based_index + 1, ".jsonl"); }
DatasetScan scan_dataset(const CompilerConfig& config, const std::vector<std::string>& splits, const mmltk::common::concurrency::CancellationObservation cancellation) {
 const auto check_cancelled = [&] {
  if (cancellation.requested()) { throw std::runtime_error("dataset compilation cancelled"); }
 };
 check_cancelled();
 const std::filesystem::path categories_path = std::filesystem::path(config.source_dir) / "categories.json";
 std::ifstream file(categories_path);
 if (!file.is_open()) { throw std::runtime_error("cannot open categories file: " + categories_path.string()); }
 json categories = json::parse(file);
 DatasetScan scan;
 struct ParsedCategory {
  std::string name;
  int raw_id = -1;
 };
 std::vector<ParsedCategory> parsed_categories;
 parsed_categories.reserve(categories["classes"].size());
 int min_raw_id = std::numeric_limits<int>::max();
 for (const auto& category : categories["classes"]) {
  check_cancelled();
  ParsedCategory parsed;
  parsed.name = category["name"].get<std::string>();
  const auto& id = category["id"];
  if (!id.is_number_integer() || (!id.is_number_unsigned() && id.get<std::int64_t>() < 0) || id.get<std::uint64_t>() > MAX_CLASSES)
   throw std::runtime_error("class id must be an exact nonnegative integer in the supported range");
  parsed.raw_id = static_cast<int>(id.get<std::uint64_t>());
  parsed_categories.push_back(std::move(parsed));
  min_raw_id = std::min(min_raw_id, parsed_categories.back().raw_id);
 }
 if (parsed_categories.empty()) { throw std::runtime_error("no classes found in categories.json"); }
 const int class_id_base = min_raw_id == 0 ? 0 : min_raw_id == 1 ? 1 : throw std::runtime_error("class ids must be dense and start at 0 or 1");
 if (parsed_categories.size() > MAX_CLASSES) throw std::runtime_error("too many classes in categories.json");
 scan.source_category_base = static_cast<std::uint8_t>(class_id_base);
 std::vector<std::string> ordered_names(parsed_categories.size());
 std::array<bool, MAX_CLASSES> seen_ids{};
 for (const auto& category : parsed_categories) {
  check_cancelled();
  const int normalized_id = category.raw_id - class_id_base;
  if (normalized_id < 0 || normalized_id >= static_cast<int>(ordered_names.size())) { throw std::runtime_error("class id out of supported range in categories.json"); }
  const auto class_id = static_cast<uint8_t>(normalized_id);
  if (seen_ids[class_id]) { throw std::runtime_error("duplicate class id in categories.json"); }
  seen_ids[class_id] = true;
  ordered_names[class_id] = category.name;
 }
 scan.class_catalog = catalog::ClassCatalog(std::move(ordered_names), COMPILED_CLASS_NAME_CAPACITY);
 scan.splits.reserve(splits.size());
 for (const std::string& split : splits) {
  check_cancelled();
  if (split.empty()) { throw std::runtime_error("dataset split name must not be empty"); }
  uint32_t num_images = 0U;
  if (categories.contains("splits") && categories["splits"].contains(split)) {
   num_images = categories["splits"][split]["total"].get<uint32_t>();
   if (num_images == 0U) { throw std::runtime_error("categories.json declares an empty dataset split: " + split); }
   mmltk::common::logging::debug([&](spdlog::logger& log) { log.debug("[compile] split '{}' count from categories.json: {} images", split, num_images); });
  } else {
   num_images = count_sequential_images(std::filesystem::path(config.source_dir) / split, cancellation);
  }
  scan.splits.push_back(DatasetCompileSplitPlan{split, num_images});
 }
 check_cancelled();
 return scan;
}
namespace {
std::size_t aligned_bytes(std::size_t bytes) { return checked_add(bytes,std::size_t{63},"annotation workspace overflow") / 64 * 64; }
std::size_t json_stack_capacity(std::size_t maximum_line) { return std::max(maximum_line,directory_line_window) / 2 + 1; }
std::size_t source_run_capacity(std::size_t maximum_line) { return std::max(maximum_line,directory_line_window) / 4 + 1; }
std::size_t padded_line_bytes(std::size_t maximum_line) { return aligned_bytes(checked_add(std::max(maximum_line,directory_line_window),simdjson::SIMDJSON_PADDING,"annotation input overflow")); }
std::size_t parser_bytes(std::size_t maximum_line) {
 return checked_add(checked_multiply(padded_line_bytes(maximum_line),std::size_t{16},"JSON parser workspace overflow"),std::size_t{64U<<10},"JSON parser workspace overflow");
}
std::uint64_t identity(const DatasetAnnotationScalar& value,const char* error) {
 if (const auto* number=value.get<std::uint64_t>()) return *number;
 if (const auto* number=value.get<std::int64_t>(); number && *number>=0) return static_cast<std::uint64_t>(*number);
 throw std::runtime_error(error);
}
double numeric(const DatasetAnnotationScalar& value,const char* error) {
 if (const auto* number=value.get<double>()) return *number;
 if (const auto* number=value.get<std::uint64_t>()) return static_cast<double>(*number);
 if (const auto* number=value.get<std::int64_t>()) return static_cast<double>(*number);
 throw std::runtime_error(error);
}
std::string_view text_value(const DatasetAnnotationScalar& value,const char* error) {
 if (const auto* text=value.get<std::string_view>()) return *text;
 throw std::runtime_error(error);
}
// The original get<int>() also admits booleans and truncates finite fractions.
int source_extent(const DatasetAnnotationScalar& value) {
 if (const auto* boolean=value.get<bool>()) return *boolean;
 if (const auto* number=value.get<std::int64_t>()) return static_cast<int>(*number);
 if (const auto* number=value.get<std::uint64_t>()) return static_cast<int>(*number);
 const auto number=numeric(value,"image_size_wh coordinates must be numeric");
 if (number<std::numeric_limits<int>::min() || number>std::numeric_limits<int>::max()) return -1;
 return static_cast<int>(number);
}
}
struct DirectoryLabels::Parser {
 detail::CompileAllowance custody;
 simdjson::ondemand::parser value;
 explicit Parser(detail::CompileExecution& execution) : custody(execution.reserve({.opaque_bytes=parser_bytes(directory_line_window)})) {
  value.threaded=false;
  if (const auto error=value.allocate(directory_line_window,json_stack_capacity(directory_line_window)+1)) throw simdjson::simdjson_error(error);
 }
};
struct DirectoryLabels::Impl {
 detail::CompiledLabelWorkspace output;
 detail::CompileAllowance custody;
 std::unique_ptr<simdjson::ondemand::parser> exceptional_parser;
 simdjson::ondemand::parser* parser = nullptr;
 common::types::WorkspaceBuffer<RLEPair> source_runs;
 common::types::WorkspaceBuffer<DatasetAnnotationFrame> json_stack;
 std::span<char> line_buffer;
 std::span<const std::uint8_t> input;
 const CompilerConfig* config = nullptr;
 const catalog::ClassCatalog* catalog = nullptr;
 std::uint8_t category_base = 0;
 std::uint32_t image_index = 0;
 std::filesystem::path annotation, image;
 dataset::MaskDimensions dimensions;
 imaging::resample::ImageResizeGeometry geometry;
 ImageEntry result{};
 std::size_t cursor = 0, line_number = 0, labels = 0, required_runs = 0;
 DatasetAnnotationJson record;
 PackedInstance label{};
 std::string_view class_name, rle;
 std::size_t run_cursor = 0, previous_end = 0, foreground = 0;
 std::optional<dataset::RowMajorMaskProjection> projection;
 dataset::RowMajorMaskBounds source_bounds;
 std::size_t projection_first = 0, mask_runs = 0;
 bool runs_complete = false, record_active = false, exceeded = false, finished = false;
 std::vector<CompileDiagnostic> diagnostics;
 [[noreturn]] void contextual(const std::exception& error) const {
  throw std::runtime_error(std::string(error.what())+" in "+annotation.string()+" at line "+std::to_string(line_number)+
   " (source "+std::to_string(dimensions.width)+"x"+std::to_string(dimensions.height)+", target "+std::to_string(config->target_width)+"x"+std::to_string(config->target_height)+")");
 }
 void open_record(std::size_t bytes) {
  std::memcpy(line_buffer.data(),input.data()+cursor,bytes);
  try {
   auto document=parser->iterate(simdjson::padded_string_view(line_buffer.data(),bytes,line_buffer.size()));
   record=project_dataset_annotation(document.get_object().value(),json_stack);
   if (!document.at_end()) throw std::runtime_error("trailing JSON content");
  } catch (const std::exception&) {
   throw std::runtime_error("invalid JSON annotation record in "+annotation.string()+" at line "+std::to_string(line_number));
  }
  if (record.image_size_wh) {
   const auto& size=*record.image_size_wh;
   if (!size.array_size || *size.array_size!=2) throw std::runtime_error("invalid image_size_wh in "+annotation.string());
   const auto width=source_extent(size.coordinates[0]), height=source_extent(size.coordinates[1]);
   if (width!=static_cast<int>(dimensions.width) || height!=static_cast<int>(dimensions.height))
    throw std::runtime_error("annotation image_size_wh does not match PNG dimensions in "+annotation.string());
  }
  if (record.image_id) {
   const auto id=identity(*record.image_id,"image_id must be a nonnegative integer");
   if (result.has_source_image_id && result.source_image_id!=id) throw std::runtime_error("inconsistent source image identities");
   result.has_source_image_id=true; result.source_image_id=id;
  }
  if (!record.class_name) throw std::runtime_error("annotation class must be a string in "+annotation.string()+" at line "+std::to_string(line_number));
  class_name=text_value(*record.class_name,"annotation class must be a string");
  const auto class_id=catalog->resolve(class_name);
  if (!class_id) throw std::runtime_error("annotation class '"+std::string(class_name)+"' is not declared in categories.json: "+annotation.string()+" at line "+std::to_string(line_number));
  label={}; label.class_id=checked_cast<std::uint8_t>(*class_id,"compiled class index overflow"); label.source_ordinal=line_number;
  try {
   const auto flag=[&](const auto& field,std::uint8_t bit) {
    if (!field) return;
    bool enabled=false;
    if (field->is_bool()) enabled=*field->template get<bool>();
    else {
     if (!field->is_int64() && !field->is_uint64()) throw std::runtime_error("annotation flag must be boolean or 0/1");
     const auto value=identity(*field,"annotation flag must be 0/1");
     if (value>1) throw std::runtime_error("annotation flag must be 0/1");
     enabled=value!=0;
    }
    if (enabled) label.flags|=bit;
   };
   flag(record.iscrowd,kAnnotationCrowd); flag(record.ignore,kAnnotationIgnore);
   const auto id=[&](const auto& field,std::uint8_t bit) -> std::uint64_t {
    if (!field) return 0;
    const auto value=identity(*field,"annotation identity must be a nonnegative integer"); label.flags|=bit; return value;
   };
   label.annotation_id=id(record.id,kAnnotationId);
   label.source_category_id=id(record.category_id,kAnnotationCategory);
   if (!(label.flags&kAnnotationCategory)) { label.source_category_id=std::uint64_t(label.class_id)+category_base; label.flags|=kAnnotationCategory; }
   rle={};
   if (record.mask_rle) {
    if (!record.mask_rle_encoding || text_value(*record.mask_rle_encoding,"mask_rle requires row_major_start_length string encoding")!="row_major_start_length")
     throw std::runtime_error("mask_rle requires row_major_start_length string encoding");
    rle=text_value(*record.mask_rle,"mask_rle requires row_major_start_length string encoding"); label.flags|=kAnnotationMask;
   }
  } catch (const std::exception& error) { contextual(error); }
  source_runs.clear(); run_cursor=0; previous_end=0; foreground=0; runs_complete=false; record_active=true;
 }
 bool parse_runs() {
  // One long source mask cannot monopolize a ready lane. The decoded string and
  // source-run array belong to this context through the next continuation.
  if (rle.empty()) return true;
  const char* begin=rle.data();
  const char* cursor=begin+run_cursor;
  const char* end=begin+rle.size();
  for (std::size_t quantum=0; cursor!=end && quantum<1024; ++quantum) {
   while (cursor!=end && std::isspace(static_cast<unsigned char>(*cursor))) ++cursor;
   if (cursor==end) break;
   std::uint32_t start=0,length=0;
   auto parsed=std::from_chars(cursor,end,start);
   if (parsed.ec!=std::errc{} || parsed.ptr==cursor) throw std::runtime_error("mask_rle contains an invalid or overflowing run start");
   cursor=parsed.ptr;
   if (cursor==end || *cursor!=':') throw std::runtime_error("mask_rle run is missing ':' separator");
   parsed=std::from_chars(++cursor,end,length);
   if (parsed.ec!=std::errc{} || parsed.ptr==cursor || !length) throw std::runtime_error("mask_rle contains an invalid, zero, or overflowing run length");
   cursor=parsed.ptr;
   if (cursor!=end && !std::isspace(static_cast<unsigned char>(*cursor))) throw std::runtime_error("mask_rle runs must be separated by whitespace");
   if (!source_runs.empty() && start<previous_end) throw std::runtime_error("mask_rle runs must be sorted and non-overlapping");
   foreground=checked_add(foreground,std::size_t(length),"mask_rle foreground count overflow");
   if (!source_runs.empty() && start==previous_end)
    source_runs.back().length=checked_cast<std::uint32_t>(std::uint64_t(source_runs.back().length)+length,"mask run length overflow");
   else source_runs.push_back({start,length});
   previous_end=std::size_t(start)+length;
  }
  run_cursor=static_cast<std::size_t>(cursor-begin);
  return cursor==end;
 }
 bool project_runs() {
  if (!projection) {
   projection_first=output.runs.size();
   const auto count=checked_add(labels,std::size_t{1},"annotation count overflow");
   const auto fits_labels=count<=output.labels.capacity();
   const auto offset=fits_labels ? annotation_layout(count,0).run_offset : COMPILED_LABEL_CAPACITY;
   const auto available=fits_labels && offset<=COMPILED_LABEL_CAPACITY ? (COMPILED_LABEL_CAPACITY-offset)/sizeof(RLEPair) : 0;
   if (!fits_labels || projection_first>available) exceeded=true;
   projection.emplace(std::span<const RLEPair>{source_runs.data(),source_runs.size()},dimensions,dataset::MaskDimensions{config->target_width,config->target_height},geometry,
    &output.scratch(),output.runs,exceeded ? projection_first : available);
  }
  try {
   if (!projection->step()) return false;
   mask_runs=output.runs.size()-projection_first;
  } catch (const dataset::MaskRunCapacityError& error) {
   mask_runs=error.required_runs()-projection_first; exceeded=true;
   // All validated source bounds survive the transactional output rollback.
   // Reusing them avoids another unbounded source scan after a capacity yield.
  }
  source_bounds=projection->source_bounds();
  return true;
 }
 void finish_record() {
  const auto first=projection_first;
  const auto count=checked_add(labels,std::size_t{1},"annotation count overflow");
  const auto& bounds=source_bounds;
  required_runs=checked_add(required_runs,mask_runs,"annotation required mask runs overflow");
  std::array<double,4> box{};
  if (record.bbox_xyxy) {
   const auto& values=*record.bbox_xyxy;
   if (!values.array_size || *values.array_size!=4) throw std::runtime_error("bbox_xyxy must have four numeric coordinates");
   for (std::size_t i=0;i<box.size();++i) box[i]=numeric(values.coordinates[i],"bbox_xyxy coordinates must be numeric");
  } else {
   if (!bounds.has_foreground) throw std::runtime_error("annotation requires a bbox or a nonempty source mask");
   box={double(bounds.min_x),double(bounds.min_y),double(bounds.max_x),double(bounds.max_y)};
  }
  if (!std::ranges::all_of(box,[](double value) { return std::isfinite(value); }) || box[2]<=box[0] || box[3]<=box[1]) throw std::runtime_error("invalid source bbox");
  if (config->diagnostics && record.mask_rle && bounds.has_foreground && record.bbox_xyxy) {
   const std::array mask_box{double(bounds.min_x),double(bounds.min_y),double(bounds.max_x),double(bounds.max_y)};
   if (mask_box!=box) try {
    diagnostics.push_back({.kind=CompileDiagnosticKind::kSourceBoundingBoxMismatch,.annotation_path=annotation.string(),.class_name=std::string(class_name),.line=line_number,
     .source_width=dimensions.width,.source_height=dimensions.height,.target_width=config->target_width,.target_height=config->target_height,.source_foreground=foreground,.declared_bbox=box,.mask_bbox=mask_box});
   } catch (...) {}
  }
  label.original_area=record.area ? numeric(*record.area,"annotation area must be numeric") : record.mask_rle ? double(foreground) : (box[2]-box[0])*(box[3]-box[1]);
  if (!std::isfinite(label.original_area) || label.original_area<0) throw std::runtime_error("invalid annotation area");
  std::array<float,4> transformed{};
  for (std::size_t i=0;i<4;++i) {
   const bool x=(i&1)==0;
   const double value=box[i]*(x ? geometry.resized_width : geometry.resized_height)/(x ? dimensions.width : dimensions.height)+(x ? geometry.offset_x : geometry.offset_y);
   constexpr auto limit=std::numeric_limits<float>::max();
   if (!std::isfinite(value) || value < -limit || value > limit) throw std::runtime_error("transformed bbox overflow");
   transformed[i]=static_cast<float>(value);
  }
  if (transformed[2]<=transformed[0] || transformed[3]<=transformed[1]) throw std::runtime_error("transformed bbox loses strict corner ordering");
  labels=count;
  if (!exceeded) {
   label.bbox_x1=transformed[0]; label.bbox_y1=transformed[1]; label.bbox_x2=transformed[2]; label.bbox_y2=transformed[3];
   label.mask_rle_offset=first*sizeof(RLEPair); label.mask_rle_pairs=checked_cast<std::uint16_t>(mask_runs,"too many RLE pairs for one instance");
   output.labels.push_back(label);
  }
  mmltk::common::logging::profile_add_value("compiler.labels.instances",1);
  mmltk::common::logging::profile_add_value("compiler.labels.rle_pairs",mask_runs);
  projection.reset(); record_active=false;
 }
};
DirectoryLabels::DirectoryLabels(detail::CompileExecution& execution) : parser_(std::make_unique<Parser>(execution)) {}
DirectoryLabels::~DirectoryLabels() = default;
detail::CompileResources DirectoryLabels::resources(std::uint32_t width,std::size_t maximum_line) {
 const auto input=padded_line_bytes(maximum_line);
 const auto source=aligned_bytes(checked_multiply(source_run_capacity(maximum_line),sizeof(RLEPair),"source mask workspace overflow"));
 const auto stack=aligned_bytes(checked_multiply(json_stack_capacity(maximum_line),sizeof(DatasetAnnotationFrame),"JSON traversal workspace overflow"));
 // simdjson owns its decoded strings, structural indexes and depth stack.
 // Payload buffers we control are supplied; the parser's fixed opaque allocation
 // is separately charged for the full lifetime of this context, including yields.
 const auto opaque=maximum_line>directory_line_window ? parser_bytes(maximum_line) : 0;
 return {.bytes=checked_add(detail::CompiledLabelWorkspace::required_bytes(width),checked_add(checked_add(input,source,"annotation workspace overflow"),stack,"annotation workspace overflow"),"annotation workspace overflow"),
  .supplied_workspace=true,.completion_workspace=true,.workspace_class=detail::CompileWorkClass::Annotations,.opaque_bytes=opaque};
}
void DirectoryLabels::begin(std::uint32_t image_index,detail::CompileAllowance allowance,std::span<const std::uint8_t> input,std::size_t maximum_line,const detail::CompileImageHeader& header,
 const CompilerConfig& config,const catalog::ClassCatalog& catalog,std::uint8_t category_base,std::filesystem::path annotation,std::filesystem::path image) {
 impl_=std::make_unique<Impl>(); auto& value=*impl_;
 value.custody=allowance; value.output.prepare(header.width,allowance);
 auto memory=allowance.workspace().subspan(detail::CompiledLabelWorkspace::required_bytes(header.width));
 const auto padded=padded_line_bytes(maximum_line);
 value.line_buffer={reinterpret_cast<char*>(memory.data()),padded}; memory=memory.subspan(padded);
 std::ranges::fill(value.line_buffer,'\0');
 value.source_runs.bind({reinterpret_cast<RLEPair*>(memory.data()),source_run_capacity(maximum_line)});
 memory=memory.subspan(aligned_bytes(checked_multiply(source_run_capacity(maximum_line),sizeof(RLEPair),"source mask workspace overflow")));
 value.json_stack.bind({reinterpret_cast<DatasetAnnotationFrame*>(memory.data()),json_stack_capacity(maximum_line)});
 value.parser=&parser_->value;
 if (maximum_line>directory_line_window) {
  value.exceptional_parser=std::make_unique<simdjson::ondemand::parser>();
  value.exceptional_parser->threaded=false;
  if (const auto error=value.exceptional_parser->allocate(maximum_line,json_stack_capacity(maximum_line)+1)) throw simdjson::simdjson_error(error);
  value.parser=value.exceptional_parser.get();
 }
 value.image_index=image_index; value.input=input; value.config=&config; value.catalog=&catalog; value.category_base=category_base; value.annotation=std::move(annotation); value.image=std::move(image);
 value.dimensions={header.width,header.height};
 value.geometry=imaging::resample::compute_image_resize_geometry(header.width,header.height,config.target_width,config.target_height,config.resize_mode);
 value.result.status=ImageStatus::Eligible; value.result.original_width=header.width; value.result.original_height=header.height;
}
bool DirectoryLabels::step(mmltk::common::concurrency::CancellationObservation cancellation) {
 auto& value=*impl_;
 for (std::size_t records=0;records<32;++records) {
  if (cancellation.requested()) throw std::runtime_error("dataset compilation cancelled");
  if (!value.record_active) {
   if (value.cursor==value.input.size()) {
    set_annotation_extent(value.result,value.labels,value.required_runs,value.image.string());
    FileHeader checks{}; checks.num_classes=checked_cast<std::uint32_t>(value.catalog->size(),"class count overflow"); checks.max_instances_per_image=UINT16_MAX;
    checks.image_width=value.config->target_width; checks.image_height=value.config->target_height;
    (void)admit_compiled_annotations(value.result,{{value.output.labels.data(),value.output.labels.size()},{value.output.runs.data(),value.output.runs.size()}},checks,value.image_index,cancellation);
    value.finished=true; return true;
   }
   const auto* begin=value.input.data()+value.cursor;
   const auto* newline=static_cast<const std::uint8_t*>(std::memchr(begin,'\n',value.input.size()-value.cursor));
   const auto bytes=newline ? static_cast<std::size_t>(newline-begin) : value.input.size()-value.cursor;
   ++value.line_number;
   if (bytes) value.open_record(bytes);
   value.cursor+=bytes+(newline ? 1 : 0);
   if (!bytes) continue;
  }
  try {
   if (!value.runs_complete) {
    if (!value.parse_runs()) return false;
    value.runs_complete=true;
   }
   if (!value.project_runs()) return false;
   value.finish_record();
  } catch (const std::exception& error) { value.contextual(error); }
 }
 return false;
}
const ImageEntry& DirectoryLabels::entry() const { return impl_->result; }
detail::CompiledLabelWorkspace::OutputLease DirectoryLabels::release_output() {
 if (!impl_ || !impl_->finished) throw std::logic_error("annotation output is not complete");
 return impl_->output.release_output();
}
void DirectoryLabels::take_diagnostics(std::vector<CompileDiagnostic>& destination) {
 for (auto& value:impl_->diagnostics) try { destination.push_back(std::move(value)); } catch (...) {}
 impl_->diagnostics.clear();
}
void DirectoryLabels::reset() noexcept { impl_.reset(); }
}  // namespace mmltk::backend::data::compiler_internal
