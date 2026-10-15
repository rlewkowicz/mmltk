module;
// Invoked by the container-owned benchmark image diagnostic. Persisted layout
// and trusted annotation views stay with the Dataset system's ordinary headers.
#include "src/backend/data/compiled/compiled_file_utils.h"
#include "src/backend/data/compiled/compiled_dataset.h"
#include "src/backend/data/compiled/compiled_format.h"
#include "src/common/concurrency/worker_pool.h"
#include "src/common/io/file_memory.h"
#include "src/common/system/execution_policy.h"
#include <fcntl.h>
#include <nlohmann/json.hpp>
export module mmltk.tools.diagnose_compiled_sample;
import std;
export int mmltk_tool_diagnose_compiled_sample(int argc, char** argv) try {
 if (argc != 3 && argc != 5) throw std::invalid_argument("expected compiled path, physical sample slot and optional opening worker count and cold-cache flag");
 namespace data = mmltk::backend::data;
 const auto parse_number = [](std::string_view text, const char* failure) {
  std::uint32_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) throw std::invalid_argument(failure);
  return value;
 };
 const auto slot = parse_number(argv[2], "invalid physical sample slot");
 const auto file = mmltk::common::io::FileHandle::open_readonly(argv[1]);
 const auto header = data::read_compiled_header(file);
 const auto sections = data::validate_compiled_file_sections(header, file.size());
 if (slot >= header.num_slots) throw std::out_of_range("sample slot is outside the compiled dataset");
 data::ImageEntry entry;
 file.pread_all(&entry, sizeof(entry), sections.index_offset + std::size_t{slot} * sizeof(entry));
 if (!entry.eligible()) throw std::invalid_argument("sample slot is not eligible");
 const auto extent = data::annotation_layout(entry.num_instances, entry.num_runs);
 if (entry.label_bytes < extent.used_bytes || entry.label_bytes > header.max_label_bytes) throw std::invalid_argument("compiled label extent differs");
 auto payload = mmltk::common::io::OwnedBuffer<std::byte>::allocate(header.max_label_bytes);
 file.pread_all(payload.data(), entry.label_bytes, sections.layout.label_offset(slot));
 const auto catalog = data::read_compiled_catalog(file, header);
 const auto annotations = data::annotation_view(entry, {payload.data(), entry.label_bytes}, &catalog);
 auto labels = nlohmann::json::array();
 std::size_t ordinal = 0;
 for (const auto& label : annotations.labels) {
  auto joints = nlohmann::json::array();
  for (const auto& joint : annotations.instance_joints(ordinal++)) joints.push_back({joint.x, joint.y, std::to_underlying(joint.visibility)});
  labels.push_back(
   {{"class_id", +label.class_id}, {"class_name", catalog.names()[label.class_id]}, {"flags", +label.flags}, {"bbox", {+label.bbox_x1, +label.bbox_y1, +label.bbox_x2, +label.bbox_y2}},
    {"mask_pairs", +label.mask_rle_pairs}, {"annotation_id", +label.annotation_id}, {"source_category_id", +label.source_category_id}, {"original_area", +label.original_area},
    {"source_ordinal", +label.source_ordinal}, {"keypoint_names", catalog.keypoints(label.class_id).names}, {"keypoints", std::move(joints)}});
 }
 nlohmann::json result{
  {"kind", "compiled_sample"}, {"path", argv[1]}, {"sample", slot}, {"image_count", +header.num_images}, {"slot_count", +header.num_slots}, {"width", +header.image_width},
  {"height", +header.image_height}, {"original_width", +entry.original_width}, {"original_height", +entry.original_height}, {"source", static_cast<std::uint8_t>(entry.source)},
  {"image_id", entry.has_source_image_id ? nlohmann::json(+entry.source_image_id) : nlohmann::json(nullptr)}, {"label_bytes", +entry.label_bytes}, {"max_label_bytes", +header.max_label_bytes},
  {"instances", std::move(labels)}
 };
 if (argc == 5) {
  const auto count = parse_number(argv[3], "invalid opening worker count");
  if (count < 1 || count > 64) throw std::invalid_argument("opening worker count must be 1–64");
  std::unique_ptr<mmltk::common::concurrency::WorkerPool> workers;
  if (count > 1) workers = std::make_unique<mmltk::common::concurrency::WorkerPool>(count, std::vector<int>{}, "compiled-open", 0, nullptr, false);
  const bool cold = std::string_view(argv[4]) == "1";
  if (cold) file.advise(0, file.size(), POSIX_FADV_DONTNEED);
  const auto started = std::chrono::steady_clock::now();
  const auto source = data::CompiledDataset::open(argv[1], data::CompiledDataset::AccessPattern::Random, {.workers = workers.get()});
  result["opening"] = {
   {"workers", workers ? workers->size() : 1U},
   {"requested_workers", count},
   {"cold", cold},
   {"duration_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started).count()},
   {"labels", source.label_count()},
   {"runs", source.run_count()},
  };
  auto policies = nlohmann::json::array();
  const auto append_policy = [&](const auto& policy) { policies.push_back({{"affinity", policy.affinity}, {"numa_node", policy.numa_node}, {"nice", policy.nice_value}}); };
  if (workers) {
   for (std::size_t worker = 0; worker < workers->size(); ++worker) append_policy(workers->policy(worker));
  } else
   append_policy(mmltk::common::system::capture_execution_policy_snapshot());
  result["opening"]["execution"] = std::move(policies);
 }
 std::cout << result.dump() << '\n';
 return 0;
} catch (const std::exception& error) {
 std::cerr << "compiled sample diagnostic error: " << error.what() << '\n';
 return 1;
}
