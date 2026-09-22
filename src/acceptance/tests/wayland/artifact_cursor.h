#pragma once
#include <cstddef>
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <nlohmann/json.hpp>
namespace mmltk::acceptance::wayland {
class JsonLineCursor final {
public:
 enum class Format { NativeJson, FirefoxText };
 JsonLineCursor(std::filesystem::path path, const std::uintmax_t offset, Format format = Format::NativeJson)
     : path_(std::move(path)), offset_(offset), format_(format), line_(offset == 0U ? std::optional<std::uint64_t>{0U} : std::nullopt) {}
 [[nodiscard]] std::optional<std::uint64_t> line() const noexcept;
 template <class Audit, class Observer>
 void consume(Audit& audit, Observer&& observer, const bool final = false) {
  std::ifstream input{path_, std::ios::binary | std::ios::ate};
  if (!input) throw std::runtime_error("evidence artifact unavailable: " + path_.string());
  const auto end = input.tellg();
  if (end < 0 || static_cast<std::uintmax_t>(end) < offset_) throw std::runtime_error("evidence artifact truncated: " + path_.string());
  input.seekg(static_cast<std::streamoff>(offset_));
  std::array<char, 16U * 1024U> chunk{};
  // Bound each allocation and consume one captured extent. Later appends
  // remain for the next notification; they cannot prolong this pass.
  const auto captured_end = static_cast<std::uintmax_t>(end);
  while (offset_ < captured_end) {
   const auto count = std::min<std::uintmax_t>(chunk.size(), captured_end - offset_);
   if (!input.read(chunk.data(), static_cast<std::streamsize>(count))) throw std::runtime_error("evidence transport read failed: " + path_.string());
   offset_ += count;
   std::string_view remaining{chunk.data(), static_cast<std::size_t>(count)};
   while (!remaining.empty()) {
    const auto newline = remaining.find('\n');
    const auto segment = newline == std::string_view::npos ? remaining.size() : newline;
    const auto admitted = std::min(segment, kLineLimit - pending_.size());
    pending_.append(remaining.data(), admitted);
    if (admitted != segment) throw std::runtime_error("evidence line exceeded capacity: " + path_.string());
    if (newline == std::string_view::npos) break;
    consume_line(audit, observer);
    pending_.clear();
    remaining.remove_prefix(segment + 1U);
   }
  }
  if (final && format_ == Format::FirefoxText && !pending_.empty()) {
   consume_line(audit, observer);
   pending_.clear();
  }
 }
 void finish() const;

private:
 template <class Audit, class Observer>
 void consume_line(Audit& audit, Observer& observer) {
  if (line_) ++*line_;
  const std::string_view line{pending_};
  if (format_ == Format::FirefoxText && ((line.contains("Uncaptured WebGPU error: Texture") && line.contains("is invalid")) || line.contains("XPCOMGlueLoad error") ||
                                         line.contains("Couldn't load XPCOM") || line.contains("panicked at"))) {
   const nlohmann::json failure{{"event", "integration.failed"}, {"detail", std::string{line}}};
   audit.consume(failure);
   observer(failure);
  }
  const auto start = format_ == Format::NativeJson ? 0U : line.find('{');
  if (start == std::string_view::npos) return;
  const auto record = nlohmann::json::parse(line.substr(start), nullptr, false);
  if (!record.is_object()) {
   if (format_ == Format::NativeJson) throw std::runtime_error("malformed native JSONL evidence: " + path_.string());
   return;
  }
  audit.consume(record);
  observer(record);
 }
 static constexpr std::size_t kLineLimit = 64U * 1024U;
 std::filesystem::path path_;
 std::uintmax_t offset_;
 Format format_;
 std::optional<std::uint64_t> line_;
 std::string pending_;
};
}  // namespace mmltk::acceptance::wayland
