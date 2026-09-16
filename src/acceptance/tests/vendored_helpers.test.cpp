#include <spdlog/details/log_msg_payload.h>
#include <spdlog/formatter.h>
#include <unistd.h>
#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../../third_party/firefox/dom/webgpu/ipc/WorkspaceSlotReleaseRegistration.h"
#include "catch2_compat.hpp"
#include "subprocess_test_utils.hpp"
namespace {
using namespace mmltk::testsupport;
using mozilla::webgpu::WorkspaceSlotReleaseRegistrationOwner;
class FixedFormatter final : public spdlog::formatter {
   public:
    explicit FixedFormatter(std::string text) : text_(std::move(text)) {}
    void format(const spdlog::details::log_msg&, spdlog::memory_buf_t& dest) override {
        dest.clear();
        dest.append(text_.data(), text_.data() + text_.size());
    }
    [[nodiscard]] std::unique_ptr<spdlog::formatter> clone() const override { return std::make_unique<FixedFormatter>(text_); }

   private:
    std::string text_;
};
std::string current_test_binary_path() {
    std::array<char, 4096> buffer{};
    const ssize_t bytes_read = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (bytes_read <= 0) { throw std::runtime_error("failed to resolve current test binary path"); }
    buffer[static_cast<std::size_t>(bytes_read)] = '\0';
    return {buffer.data()};
}
SubprocessResult run_reporter_fixture(const std::string& reporter_name) {
    return run_subprocess_capture_output({
        current_test_binary_path(),
        "vendored_catch2_reporter_fixture",
        "--reporter",
        reporter_name,
        "--colour-mode",
        "none",
    });
}
void test_spdlog_log_msg_payload_helpers_preserve_raw_payload_and_apply_formatting() {
    spdlog::details::log_msg msg("vendored-helper", spdlog::level::info, "raw payload");
    FixedFormatter formatter("formatted payload");
    spdlog::memory_buf_t formatted;
    const spdlog::string_view_t raw_payload = spdlog::details::format_log_msg_payload(false, formatter, msg, formatted);
    MMLTK_ASSERT(raw_payload == msg.payload);
    MMLTK_ASSERT(formatted.size() == 0U);
    MMLTK_ASSERT(spdlog::details::log_msg_payload_length(raw_payload) == static_cast<int>(msg.payload.size()));
    const spdlog::string_view_t formatted_payload = spdlog::details::format_log_msg_payload(true, formatter, msg, formatted);
    MMLTK_ASSERT(formatted_payload == spdlog::string_view_t("formatted payload", 17));
    MMLTK_ASSERT(spdlog::details::log_msg_payload_length(formatted_payload) == 17);
}
void test_spdlog_log_msg_payload_helpers_clamp_large_lengths_to_int_max() {
    constexpr std::array<char, 2> kSentinel{'x', '\0'};
    const std::size_t oversized_length = static_cast<std::size_t>(std::numeric_limits<int>::max()) + 128U;
    const spdlog::string_view_t oversized_payload(kSentinel.data(), oversized_length);
    MMLTK_ASSERT(spdlog::details::log_msg_payload_length(oversized_payload) == std::numeric_limits<int>::max());
}
void test_catch2_compact_reporter_formats_shared_assertion_details() {
    const SubprocessResult result = run_reporter_fixture("compact");
    MMLTK_ASSERT(result.exit_code != 0);
    MMLTK_ASSERT(result.output_text.find("failed: fixture_value == 2") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("for: 1 == 2") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("with 1 message: 'vendored reporter info'") != std::string::npos);
}
void test_catch2_tap_reporter_formats_shared_assertion_details() {
    const SubprocessResult result = run_reporter_fixture("tap");
    MMLTK_ASSERT(result.exit_code != 0);
    MMLTK_ASSERT(result.output_text.find("# vendored_catch2_reporter_fixture") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("not ok 1 - fixture_value == 2") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("for: 1 == 2") != std::string::npos);
    MMLTK_ASSERT(result.output_text.find("with 1 message: 'vendored reporter info'") != std::string::npos);
}
void test_workspace_slot_release_registration_shares_one_window_listener_until_last_device() {
    WorkspaceSlotReleaseRegistrationOwner registrations;
    const auto first = registrations.Register(1U, 11U);
    MMLTK_ASSERT(first.attach_window == 11U);
    MMLTK_ASSERT(!first.detach_window);
    const auto second = registrations.Register(2U, 11U);
    MMLTK_ASSERT(!second.attach_window);
    MMLTK_ASSERT(!second.detach_window);
    MMLTK_ASSERT(registrations.DeviceCount(11U) == 2U);
    MMLTK_ASSERT(registrations.WindowCount() == 1U);
    MMLTK_ASSERT(!registrations.Unregister(1U));
    MMLTK_ASSERT(registrations.DeviceCount(11U) == 1U);
    MMLTK_ASSERT(registrations.Unregister(2U) == 11U);
    MMLTK_ASSERT(registrations.DeviceCount() == 0U);
    MMLTK_ASSERT(registrations.WindowCount() == 0U);
}
void test_workspace_slot_release_registration_tracks_windows_independently() {
    WorkspaceSlotReleaseRegistrationOwner registrations;
    MMLTK_ASSERT(registrations.Register(1U, 11U).attach_window == 11U);
    MMLTK_ASSERT(registrations.Register(2U, 22U).attach_window == 22U);
    MMLTK_ASSERT(registrations.WindowCount() == 2U);
    MMLTK_ASSERT(registrations.Unregister(1U) == 11U);
    MMLTK_ASSERT(registrations.WindowCount() == 1U);
    MMLTK_ASSERT(registrations.DeviceCount(22U) == 1U);
    MMLTK_ASSERT(registrations.Unregister(2U) == 22U);
}
void test_workspace_slot_release_registration_tolerates_duplicate_and_reordered_unregister() {
    WorkspaceSlotReleaseRegistrationOwner registrations;
    MMLTK_ASSERT(registrations.Register(1U, 11U).attach_window == 11U);
    const auto duplicate = registrations.Register(1U, 11U);
    MMLTK_ASSERT(!duplicate.attach_window);
    MMLTK_ASSERT(!duplicate.detach_window);
    MMLTK_ASSERT(registrations.DeviceCount(11U) == 1U);
    const auto moved = registrations.Register(1U, 22U);
    MMLTK_ASSERT(moved.detach_window == 11U);
    MMLTK_ASSERT(moved.attach_window == 22U);
    MMLTK_ASSERT(registrations.DeviceCount(11U) == 0U);
    MMLTK_ASSERT(registrations.DeviceCount(22U) == 1U);
    MMLTK_ASSERT(registrations.Unregister(1U) == 22U);
    MMLTK_ASSERT(!registrations.Unregister(1U));
}
void test_workspace_slot_release_registration_handles_window_death_and_system_teardown() {
    WorkspaceSlotReleaseRegistrationOwner registrations;
    MMLTK_ASSERT(registrations.Register(1U, 11U).attach_window == 11U);
    MMLTK_ASSERT(!registrations.Register(2U, 11U).attach_window);
    MMLTK_ASSERT(registrations.Register(3U, 22U).attach_window == 22U);
    MMLTK_ASSERT(registrations.RemoveWindow(11U));
    MMLTK_ASSERT(!registrations.RemoveWindow(11U));
    MMLTK_ASSERT(!registrations.Unregister(1U));
    MMLTK_ASSERT(!registrations.Unregister(2U));
    MMLTK_ASSERT(registrations.DeviceCount() == 1U);
    MMLTK_ASSERT(registrations.WindowCount() == 1U);
    registrations.Clear();
    registrations.Clear();
    MMLTK_ASSERT(!registrations.Unregister(3U));
    MMLTK_ASSERT(registrations.DeviceCount() == 0U);
    MMLTK_ASSERT(registrations.WindowCount() == 0U);
}
}  // namespace
TEST_CASE("vendored_catch2_reporter_fixture", "[.][core][vendored][reporter_fixture]") {
    const int fixture_value = 1;
    INFO("vendored reporter info");
    // NOLINTNEXTLINE(bugprone-chained-comparison): Catch2 decomposes REQUIRE through operator<=.
    REQUIRE(fixture_value == 2);
}
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_spdlog_log_msg_payload_helpers_preserve_raw_payload_and_apply_formatting);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_spdlog_log_msg_payload_helpers_clamp_large_lengths_to_int_max);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_catch2_compact_reporter_formats_shared_assertion_details);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_catch2_tap_reporter_formats_shared_assertion_details);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_workspace_slot_release_registration_shares_one_window_listener_until_last_device);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_workspace_slot_release_registration_tracks_windows_independently);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_workspace_slot_release_registration_tolerates_duplicate_and_reordered_unregister);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_workspace_slot_release_registration_handles_window_death_and_system_teardown);
