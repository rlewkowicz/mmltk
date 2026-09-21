#include "src/common/io/json_file.h"
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
namespace mmltk::common::io {
std::optional<JsonWriteStage> append_json_line(const std::filesystem::path& path, const nlohmann::json& payload) {
 std::ofstream stream(path, std::ios::app);
 if (!stream.is_open()) return JsonWriteStage::kOpen;
 stream << payload.dump() << '\n';
 stream.close();
 return stream ? std::nullopt : std::optional{JsonWriteStage::kFlush};
}
std::optional<JsonWriteStage> write_json_text_file_atomic(const std::filesystem::path& path, const std::string_view encoded) {
 if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) return JsonWriteStage::kFlush;
 const std::filesystem::path temporary = path.string() + ".tmp";
 {
  std::ofstream stream(temporary);
  if (!stream.is_open()) return JsonWriteStage::kOpen;
  stream.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
  stream.put('\n');
  stream.close();
  if (!stream) return JsonWriteStage::kFlush;
 }
 std::error_code error;
 std::filesystem::rename(temporary, path, error);
 return error ? std::optional{JsonWriteStage::kRename} : std::nullopt;
}
std::optional<JsonWriteStage> write_json_file_atomic(const std::filesystem::path& path, const nlohmann::json& payload, const int indent) {
 return write_json_text_file_atomic(path, payload.dump(indent));
}
const char* to_string(const JsonWriteStage stage) noexcept {
 switch (stage) {
  case JsonWriteStage::kOpen: return "open";
  case JsonWriteStage::kFlush: return "flush";
  case JsonWriteStage::kRename: return "rename";
 }
 return "unknown";
}
void throw_on_json_write_failure(const std::optional<JsonWriteStage> failure, const std::filesystem::path& path, const std::string_view operation) {
 if (failure) throw std::runtime_error("failed to " + std::string(to_string(*failure)) + " " + std::string(operation) + ": " + path.string());
}
}  // namespace mmltk::common::io
