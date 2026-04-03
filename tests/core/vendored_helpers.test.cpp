#include "support/catch2_compat.hpp"
#include "support/subprocess_test_utils.hpp"

#include <spdlog/details/log_msg_payload.h>
#include <spdlog/formatter.h>

#include <array>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

using namespace mmltk::testsupport;

class FixedFormatter final : public spdlog::formatter {
public:
    explicit FixedFormatter(std::string text)
        : text_(std::move(text)) {}

    void format(const spdlog::details::log_msg&, spdlog::memory_buf_t& dest) override {
        dest.clear();
        dest.append(text_.data(), text_.data() + text_.size());
    }

    [[nodiscard]] std::unique_ptr<spdlog::formatter> clone() const override {
        return std::make_unique<FixedFormatter>(text_);
    }

private:
    std::string text_;
};

std::string current_test_binary_path() {
    std::array<char, 4096> buffer{};
    const ssize_t bytes_read = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1U);
    if (bytes_read <= 0) {
        throw std::runtime_error("failed to resolve current test binary path");
    }
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

    const spdlog::string_view_t raw_payload =
        spdlog::details::format_log_msg_payload(false, formatter, msg, formatted);
    assert(raw_payload == msg.payload);
    assert(formatted.size() == 0U);
    assert(spdlog::details::log_msg_payload_length(raw_payload) == static_cast<int>(msg.payload.size()));

    const spdlog::string_view_t formatted_payload =
        spdlog::details::format_log_msg_payload(true, formatter, msg, formatted);
    assert(formatted_payload == spdlog::string_view_t("formatted payload", 17));
    assert(spdlog::details::log_msg_payload_length(formatted_payload) == 17);
}

void test_spdlog_log_msg_payload_helpers_clamp_large_lengths_to_int_max() {
    constexpr std::array<char, 2> kSentinel{'x', '\0'};
    const std::size_t oversized_length =
        static_cast<std::size_t>(std::numeric_limits<int>::max()) + 128U;
    const spdlog::string_view_t oversized_payload(kSentinel.data(), oversized_length);

    assert(spdlog::details::log_msg_payload_length(oversized_payload) ==
           std::numeric_limits<int>::max());
}

void test_catch2_compact_reporter_formats_shared_assertion_details() {
    const SubprocessResult result = run_reporter_fixture("compact");

    assert(result.exit_code != 0);
    assert(result.output_text.find("failed: fixture_value == 2") != std::string::npos);
    assert(result.output_text.find("for: 1 == 2") != std::string::npos);
    assert(result.output_text.find("with 1 message: 'vendored reporter info'") != std::string::npos);
}

void test_catch2_tap_reporter_formats_shared_assertion_details() {
    const SubprocessResult result = run_reporter_fixture("tap");

    assert(result.exit_code != 0);
    assert(result.output_text.find("# vendored_catch2_reporter_fixture") != std::string::npos);
    assert(result.output_text.find("not ok 1 - fixture_value == 2") != std::string::npos);
    assert(result.output_text.find("for: 1 == 2") != std::string::npos);
    assert(result.output_text.find("with 1 message: 'vendored reporter info'") != std::string::npos);
}

} // namespace

TEST_CASE("vendored_catch2_reporter_fixture", "[.][core][vendored][reporter_fixture]") {
    const int fixture_value = 1;
    INFO("vendored reporter info");
    REQUIRE(fixture_value == 2);
}

MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_spdlog_log_msg_payload_helpers_preserve_raw_payload_and_apply_formatting);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_spdlog_log_msg_payload_helpers_clamp_large_lengths_to_int_max);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_catch2_compact_reporter_formats_shared_assertion_details);
MMLTK_REGISTER_TEST_CASE("[core][vendored]", test_catch2_tap_reporter_formats_shared_assertion_details);
