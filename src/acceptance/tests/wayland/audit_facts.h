#pragma once
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <nlohmann/json.hpp>
namespace mmltk::acceptance::wayland {
void append_acceptance_record(const std::filesystem::path& path, nlohmann::json record);
constexpr std::size_t kAcceptanceGenerationLimit = 128U;
// Numeric entry observes admission and completion snapshots for each digit.
constexpr std::size_t kAcceptanceSnapshotLimit = 256U;
constexpr std::size_t kAcceptanceRecordLimit = 4096U;
constexpr const char* kExploreGalleryControl = "explore.gallery.workspace";
[[nodiscard]] constexpr std::string_view first_failed_check() noexcept { return {}; }
template <typename... Remaining>
[[nodiscard]] constexpr std::string_view first_failed_check(const bool passed, const std::string_view label, Remaining&&... remaining) noexcept {
 return passed ? first_failed_check(std::forward<Remaining>(remaining)...) : label;
}
[[nodiscard]] inline std::uint64_t scalar(const nlohmann::json& value, const char* const field) noexcept {
 const auto found = value.find(field);
 if (found == value.end()) return 0U;
 if (found->is_number_unsigned()) return found->get<std::uint64_t>();
 if (!found->is_string()) return 0U;
 const std::string& text = found->get_ref<const std::string&>();
 std::uint64_t result = 0U;
 const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
 return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0U;
}
[[nodiscard]] inline std::string_view textual(const nlohmann::json& value, const char* const field) noexcept {
 const auto found = value.find(field);
 return found != value.end() && found->is_string() ? std::string_view{found->get_ref<const std::string&>()} : std::string_view{};
}
[[nodiscard]] inline double numeric(const nlohmann::json& value, const char* const field) noexcept {
 const auto found = value.find(field);
 if (found == value.end()) return 0.0;
 if (found->is_number()) return found->get<double>();
 if (!found->is_string()) return 0.0;
 const std::string& text = found->get_ref<const std::string&>();
 double result = 0.0;
 const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
 return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? result : 0.0;
}
// Native and browser files are drained independently. Validate ordering within
// each producer stream, then join their facts by the physical capability; file
// read order is not a cross-process happens-before relation.
class FirstAuditFailure final {
public:
 template <class Context>
 void capture(const std::string_view reason, const nlohmann::json& observed, Context&& context) {
  if (!record_.is_null()) return;
  record_ = observed;
  record_["observed_event"] = observed.value("event", "");
  record_["reason"] = reason;
  record_["audit_context"] = context();
 }
 void report(const std::filesystem::path& path, const std::string_view event, const std::filesystem::path& observed_file, const std::optional<std::uint64_t> observed_line) {
  if (record_.is_null() || reported_) return;
  auto record = record_;
  record["event"] = event;
  record["observed_file"] = observed_file.string();
  if (observed_line) record["observed_line"] = *observed_line;
  append_acceptance_record(path, std::move(record));
  reported_ = true;
 }
 [[nodiscard]] const nlohmann::json& record() const noexcept { return record_; }

private:
 nlohmann::json record_;
 bool reported_ = false;
};
}  // namespace mmltk::acceptance::wayland
