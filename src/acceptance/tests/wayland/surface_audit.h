#pragma once
#include <nlohmann/json.hpp>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>
namespace mmltk::acceptance::wayland {
struct SurfaceAudit final {
 struct Reconstruction final {
  std::string completed;
  std::string requested;
  std::uint64_t publication = 0U;
  std::size_t ordinal = 0U;
 };
 struct SourceRead final {
  std::string source;
  std::uint64_t allocation = 0U;
  std::uint64_t session = 0U;
  std::uint64_t frame = 0U;
  std::uint64_t width = 0U;
  std::uint64_t height = 0U;
  bool operator==(const SourceRead&) const = default;
 };
 enum class TerminalRead { None, Completed, Retained };
 struct MetadataReceipt final {
  std::uint64_t bytes = 0U;
  std::string fingerprint;
  bool operator==(const MetadataReceipt&) const = default;
 };
 struct TransferReceipt final {
  SourceRead read;
  bool releasing = false;
  bool released = false;
  TerminalRead terminal = TerminalRead::None;
  std::optional<MetadataReceipt> metadata{};
 };
 struct ReceiverReceipt final {
  std::string source;
  std::uint64_t transfer = 0U;
  std::uint64_t layer = 0U;
  std::uint64_t slot = 0U;
  std::uint64_t session = 0U;
  std::uint64_t frame = 0U;
  std::uint64_t width = 0U;
  std::uint64_t height = 0U;
  unsigned stage = 0U;
  bool direct_sampling = false;
  bool release_only = false;
 };
 struct SampleCustody final {
  std::uint64_t selected = 0U;
  std::uint64_t encoded = 0U;
  std::uint64_t settled = 0U;
  std::uint64_t abandoned = 0U;
  bool acquired = false;
  std::size_t acquired_at = 0U;
  bool released = false;
  std::optional<MetadataReceipt> metadata;
 };
 struct SurfaceState final {
  std::uint64_t generation = 0U;
  std::uint64_t width = 0U;
  std::uint64_t height = 0U;
  std::uint64_t browser_width = 0U;
  std::uint64_t browser_height = 0U;
  unsigned native_stage = 0U;
  // CLEANUP-IGNORE: Firefox import lifecycle state is distinct from the browser-controller outcome flags.
  unsigned firefox_stage = 0U;
  bool import_failed = false;
  bool firefox_import_failed = false;
  bool withdrawn = false;
  bool candidate_withdrawn = false;
  bool native_retired = false;
  std::size_t firefox_withdrawn = 0U;
  std::size_t firefox_retired = 0U;
  std::size_t admitted = 0U;
  std::size_t created = 0U;
  std::size_t acquired = 0U;
  std::size_t discarded = 0U;
  std::size_t retired = 0U;
  std::optional<Reconstruction> reconstruction;
  std::map<std::uint64_t, SourceRead> reads;
  std::map<std::pair<std::uint64_t, std::uint64_t>, TransferReceipt> transfers;
  std::map<std::uint64_t, ReceiverReceipt> receipts;
  std::map<std::uint64_t, SampleCustody> custody;
  std::unordered_map<std::uint64_t, std::size_t> draw_indices;
  std::map<std::string, unsigned, std::less<>> source_textures;
  std::size_t import_dropped = 0U;
  std::map<std::uint64_t, unsigned> source_steps;
  std::map<std::pair<std::uint64_t, std::uint64_t>, unsigned> publication_steps;
  std::set<std::uint64_t> failed_source_operations;
  std::set<std::pair<std::uint64_t, std::uint64_t>> failed_publications;
  std::map<std::uint64_t, std::uint64_t> ended_spans;
  std::map<std::uint64_t, std::uint64_t> publications;
  std::map<std::uint64_t, std::uint64_t> samples;
 };
 class SourceLifecycle final {
 public:
  [[nodiscard]] bool Observe(const unsigned step, const bool shutdown) {
   if (retired_) return false;
   if (step <= 3U) {
    if (cancelled_ || admission_ + 1U != step) return false;
    admission_ = step;
    return true;
   }
   if (admission_ == 0U) return false;
   if (step == 4U) {
    if (withdrawn_) return false;
    withdrawn_ = true;
    return true;
   }
   if (step != 5U || (!withdrawn_ && !shutdown)) return false;
   retired_ = true;
   return true;
  }
  [[nodiscard]] bool ready() const noexcept { return admission_ == 3U; }
  [[nodiscard]] bool live() const noexcept { return ready() && !withdrawn_ && !retired_; }
  [[nodiscard]] bool retired() const noexcept { return retired_; }
  [[nodiscard]] bool cancelled() const noexcept { return cancelled_; }
  [[nodiscard]] bool Cancel() noexcept {
   if (admission_ == 0U || ready() || !withdrawn_ || retired_ || cancelled_) return false;
   cancelled_ = true;
   return true;
  }

 private:
  unsigned admission_ = 0U;
  bool withdrawn_ = false;
  bool retired_ = false;
  bool cancelled_ = false;
 };
 struct SourceState final {
  SourceLifecycle native;
  SourceLifecycle browser;
  // CLEANUP-IGNORE: This independent source lifecycle oracle compares native and browser evidence; it cannot reuse their production
  // schema.
  bool failed = false;
  // CLEANUP-IGNORE: Independent observed source dimensions and allocation identity are acceptance evidence, not shared runtime
  // storage.
  std::uint64_t generation = 0U;
  std::uint64_t width = 0U;
  std::uint64_t height = 0U;
  std::uint64_t allocation = 0U;
  std::uint64_t bytes = 0U;
  std::uint64_t pitch = 0U;
  std::uint64_t browser_width = 0U;
  std::uint64_t browser_height = 0U;
  std::uint64_t browser_allocation = 0U;
  std::string arena;
  bool direct_sampling = false;
  bool browser_direct_sampling = false;
 };
 std::map<std::string, SourceState, std::less<>> sources;
 void source_transition(const std::string& id, const std::string_view event, const bool browser, const std::uint64_t code = 0U);
 struct Draw final {
  std::string selected;
  std::string requested;
  std::uint64_t publication = 0U;
  std::size_t ordinal = 0U;
  std::size_t encoded = 0U;
  std::size_t submitted = 0U;
  std::uint64_t identity = 0U;
  std::size_t settled = 0U;
  bool abandoned = false;
 };
 std::map<std::string, SurfaceState, std::less<>> surfaces;
 std::vector<Draw> draws;
 std::string failure;
 std::size_t browser_ordinal = 0U;
 std::size_t scenario_started = 0U;
 bool native_shutdown = false;
 bool browser_shutdown = false;
 bool browser_exited = false;
 void reject(const std::string_view why);
 [[nodiscard]] bool admit(const std::string& id);
 static std::string native_identity(const nlohmann::json& record, const bool workspace = false);
 static bool valid_identity(const std::string_view id);
 [[nodiscard]] std::optional<MetadataReceipt> metadata_receipt(const nlohmann::json& record);
 [[nodiscard]] static bool valid_transfer_timeline(const nlohmann::json& record, const std::uint64_t transfer);
 void native(const nlohmann::json& record);
 void browser(const nlohmann::json& record);
 [[nodiscard]] bool source_joined(const SourceState& source) const;
 [[nodiscard]] bool receipts_joined(const std::string& id, const SurfaceState& state, const bool permit_held = false) const;
 [[nodiscard]] std::string joined_failure() const;
 [[nodiscard]] std::string joined_surface_failure(const std::string& id, const SurfaceState& state) const;
 [[nodiscard]] bool evidence_settled(std::string* blocker = nullptr) const;
 void SettleScenario();
 [[nodiscard]] const SampleCustody* settled_draw(const Draw& draw) const;
 [[nodiscard]] const Draw* pending_fallback(const std::string& candidate) const;
 [[nodiscard]] bool pending_supersession_completed() const;
};
}  // namespace mmltk::acceptance::wayland
