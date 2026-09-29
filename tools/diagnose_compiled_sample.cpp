// Invoked by the container-owned benchmark image diagnostic. Persisted layout
// and annotation admission stay with the Dataset system's ordinary headers.
#include "src/backend/data/compiled/compiled_file_utils.h"
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>
int main(int argc, char** argv) try {
 if (argc != 3) throw std::invalid_argument("expected compiled path and physical sample slot");
 namespace data = mmltk::backend::data;
 const std::string_view sample(argv[2]);
 std::uint32_t slot = 0;
 const auto parsed = std::from_chars(sample.data(), sample.data() + sample.size(), slot);
 if (parsed.ec != std::errc{} || parsed.ptr != sample.data() + sample.size()) throw std::invalid_argument("invalid physical sample slot");
 const auto file = mmltk::common::io::FileHandle::open_readonly(argv[1]);
 const auto header = data::read_compiled_header(file);
 const auto sections = data::validate_compiled_file_sections(header, file.size());
 if (slot >= header.num_slots) throw std::out_of_range("sample slot is outside the compiled dataset");
 data::ImageEntry entry;
 file.pread_all(&entry, sizeof(entry), sections.index_offset + std::size_t{slot} * sizeof(entry));
 if (!entry.eligible()) throw std::invalid_argument("sample slot is not eligible");
 const auto extent = data::annotation_layout(entry.num_instances, entry.num_runs);
 if (entry.label_bytes != extent.used_bytes || entry.label_bytes > header.max_label_bytes) throw std::invalid_argument("compiled label extent differs");
 auto payload = mmltk::common::io::OwnedBuffer<std::byte>::allocate(header.max_label_bytes);
 file.pread_all(payload.data(), extent.used_bytes, sections.layout.label_offset(slot));
 const auto annotations = data::annotation_view(entry, {payload.data(), extent.used_bytes});
 (void)data::admit_compiled_annotations(entry, annotations, header, slot);
 const auto catalog = data::compiled_class_catalog(header);
 auto labels = nlohmann::json::array();
 for (const auto& label : annotations.labels) {
  labels.push_back(
   {{"class_id", +label.class_id}, {"class_name", catalog.names()[label.class_id]}, {"flags", +label.flags}, {"bbox", {+label.bbox_x1, +label.bbox_y1, +label.bbox_x2, +label.bbox_y2}},
    {"mask_pairs", +label.mask_rle_pairs}, {"annotation_id", +label.annotation_id}, {"source_category_id", +label.source_category_id}, {"original_area", +label.original_area},
    {"source_ordinal", +label.source_ordinal}});
 }
 const nlohmann::json result{
  {"kind", "compiled_sample"}, {"path", argv[1]}, {"sample", slot}, {"image_count", +header.num_images}, {"slot_count", +header.num_slots}, {"width", +header.image_width},
  {"height", +header.image_height}, {"original_width", +entry.original_width}, {"original_height", +entry.original_height}, {"source", static_cast<std::uint8_t>(entry.source)},
  {"image_id", entry.has_source_image_id ? nlohmann::json(+entry.source_image_id) : nlohmann::json(nullptr)}, {"label_bytes", +entry.label_bytes}, {"max_label_bytes", +header.max_label_bytes},
  {"instances", std::move(labels)}
 };
 std::cout << result.dump() << '\n';
 return 0;
} catch (const std::exception& error) {
 std::cerr << "compiled sample diagnostic: " << error.what() << '\n';
 return 1;
}
