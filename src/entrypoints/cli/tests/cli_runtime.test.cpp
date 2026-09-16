#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <meta>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "mmltk/frameworks/reflection/materializer.h"
#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_descriptors.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/test_support/subprocess_test_utils.hpp"
#include "src/test_support/filesystem_test_utils.hpp"
namespace {
thread_local bool g_count_cli_allocations = false;
thread_local std::size_t g_cli_allocation_count = 0U;
}  // namespace
[[gnu::noinline]] void* operator new(const std::size_t size) {
    if (g_count_cli_allocations) ++g_cli_allocation_count;
    if (void* const allocation = std::malloc(size == 0U ? 1U : size); allocation != nullptr) return allocation;
    throw std::bad_alloc();
}
[[gnu::noinline]] void* operator new[](const std::size_t size) {
    if (g_count_cli_allocations) ++g_cli_allocation_count;
    if (void* const allocation = std::malloc(size == 0U ? 1U : size); allocation != nullptr) return allocation;
    throw std::bad_alloc();
}
[[gnu::noinline]] void operator delete(void* const allocation) noexcept { std::free(allocation); }
[[gnu::noinline]] void operator delete[](void* const allocation) noexcept { std::free(allocation); }
[[gnu::noinline]] void operator delete(void* const allocation, std::size_t) noexcept { std::free(allocation); }
[[gnu::noinline]] void operator delete[](void* const allocation, std::size_t) noexcept { std::free(allocation); }
namespace {
using namespace mmltk::testsupport;
namespace fs = std::filesystem;
fs::path make_temp_dir(const char* label) {
    const fs::path dir = fs::temp_directory_path() / (std::string(label) + "-" + std::to_string(static_cast<long long>(::getpid())));
    std::error_code error;
    fs::create_directories(dir, error);
    REQUIRE((!error));
    return dir;
}
void cleanup_temp_dir(const fs::path& dir) {
    std::error_code error;
    fs::remove_all(dir, error);
}
std::vector<std::string> read_text_lines(const fs::path& path) {
    std::ifstream stream(path);
    REQUIRE((stream.is_open()));
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) { lines.push_back(line); }
    return lines;
}
fs::path locate_repo_wrapper_path() {
    auto find_repo_wrapper = [](fs::path current, const fs::path& cli_path) -> fs::path {
        for (int depth = 0; depth < 8; ++depth) {
            const fs::path candidate = current / "mmltk";
            std::error_code error;
            if (fs::is_regular_file(candidate, error) && !error && candidate != cli_path && fs::exists(current / "CMakeLists.txt") &&
                fs::exists(current / "README.md")) {
                return candidate;
            }
            if (!current.has_parent_path() || current.parent_path() == current) { break; }
            current = current.parent_path();
        }
        return {};
    };
    const fs::path cli_path = mmltk_cli_path();
    if (const char* repo_root = std::getenv("MMLTK_REPO_ROOT"); repo_root != nullptr && repo_root[0] != '\0') {
        const fs::path wrapper = find_repo_wrapper(fs::path(repo_root), cli_path);
        if (!wrapper.empty()) { return wrapper; }
    }
    {
        const fs::path wrapper = find_repo_wrapper(fs::current_path(), cli_path);
        if (!wrapper.empty()) { return wrapper; }
    }
    {
        const fs::path wrapper = find_repo_wrapper(cli_path.parent_path(), cli_path);
        if (!wrapper.empty()) { return wrapper; }
    }
    throw std::runtime_error("failed to locate repo-root mmltk wrapper");
}
fs::path write_fake_docker_script(const fs::path& root) {
    const fs::path bin_dir = root / "bin";
    const fs::path script_path = bin_dir / "docker";
    std::error_code error;
    fs::create_directories(bin_dir, error);
    REQUIRE((!error));
    std::ofstream stream(script_path, std::ios::trunc);
    REQUIRE((stream.is_open()));
    stream << "#!/usr/bin/env bash\n"
              "set -euo pipefail\n"
              "state_dir=\"${MMLTK_FAKE_DOCKER_STATE:?}\"\n"
              "command=\"${1:-}\"\n"
              "shift || true\n"
              "case \"${command}\" in\n"
              "version)\n"
              "    exit 0\n"
              "    ;;\n"
              "image)\n"
              "    if [[ \"${1:-}\" == \"inspect\" ]]; then\n"
              "        if [[ \" ${*} \" == *\" --format \"* ]]; then\n"
              "            printf 'sha256:fake-image\\n'\n"
              "        fi\n"
              "        exit 0\n"
              "    fi\n"
              "    ;;\n"
              "container)\n"
              "    if [[ \"${1:-}\" == \"inspect\" ]]; then\n"
              "        exit 1\n"
              "    fi\n"
              // Cleanup re-checks for a surviving container by name after removal.
              // This fake never creates one, so an empty listing is the truthful
              // answer; falling through to the unrecognised-command arm instead
              // reads as a cleanup failure and fails the whole invocation.
              "    if [[ \"${1:-}\" == \"ls\" ]]; then\n"
              "        exit 0\n"
              "    fi\n"
              "    ;;\n"
              "run)\n"
              "    printf '%s\\n' \"$@\" > \"${state_dir}/run_args.txt\"\n"
              "    exit 0\n"
              "    ;;\n"
              "exec)\n"
              "    count=0\n"
              "    if [[ -f \"${state_dir}/exec_count.txt\" ]]; then\n"
              "        count=\"$(cat \"${state_dir}/exec_count.txt\")\"\n"
              "    fi\n"
              "    count=$((count + 1))\n"
              "    printf '%s' \"${count}\" > \"${state_dir}/exec_count.txt\"\n"
              "    printf '%s\\n' \"$@\" > \"${state_dir}/exec_${count}_args.txt\"\n"
              "    case \" $* \" in\n"
              "        *\" wasm_ref=\"*) printf '/opt/mmltk/browser-app/mmltk_browser_app_bg.wasm\\n' ;;\n"
              "    esac\n"
              "    exit 0\n"
              "    ;;\n"
              "start|rm|inspect)\n"
              "    exit 0\n"
              "    ;;\n"
              "esac\n"
              "printf 'unexpected docker command: %s\\n' \"${command}\" >&2\n"
              "exit 1\n";
    stream.close();
    fs::permissions(script_path, fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec, fs::perm_options::replace, error);
    REQUIRE((!error));
    return script_path;
}
std::string prepend_path_env(const fs::path& prefix_dir) {
    std::string value = prefix_dir.string();
    if (const char* current_path = std::getenv("PATH"); current_path != nullptr && current_path[0] != '\0') {
        value += ":";
        value += current_path;
    }
    return value;
}
void assert_contains_line(const std::vector<std::string>& lines, const std::string& expected) {
    REQUIRE((std::find(lines.begin(), lines.end(), expected) != lines.end()));
}
void assert_contains_substring(const std::vector<std::string>& lines, const std::string& expected) {
    const auto match = std::find_if(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(expected) != std::string::npos; });
    REQUIRE((match != lines.end()));
}
enum class ParserMode : std::uint8_t {
    Fast,
    FullTrace,
};
struct ParserNestedState {
    [[= mmltk::frameworks::reflection::Minimum<int>{0}]][[= mmltk::frameworks::reflection::Maximum<int>{10}]] int count = 0;
    bool operator==(const ParserNestedState&) const = default;
};
struct ParserRequest {
    ParserNestedState nested;
    [[= mmltk::frameworks::reflection::Minimum<std::uint32_t>{
        1U}]][[= mmltk::frameworks::reflection::Maximum<std::uint32_t>{4U}]] std::uint32_t unsigned_count = 1U;
    [[= mmltk::frameworks::reflection::Minimum<double>{
        0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]] double ratio = 0.5;
    [[= mmltk::frameworks::reflection::MaxBytes{4U}]] std::string label;
    bool enabled = true;
    [[= mmltk::frameworks::reflection::MaxBytes{mmltk::frameworks::reflection::kMaximumPathBytes}]] std::optional<fs::path> optional_path;
    [[= mmltk::frameworks::reflection::MaxItems{2U}]] std::vector<int> values;
    ParserMode mode = ParserMode::Fast;
    int positional = 0;
    bool operator==(const ParserRequest&) const = default;
};
MMLTK_REFLECT_FIELDS(ParserNestedState)
MMLTK_REFLECT_FIELDS(ParserRequest)
MMLTK_REFLECT_ENUM(ParserMode)
struct InheritedCliBase {
    [[= mmltk::frameworks::reflection::Minimum<std::int32_t>{1}]][[= mmltk::frameworks::reflection::Maximum<std::int32_t>{9}]] std::int32_t inherited_limit = 4;
};
struct InheritedCliRequest final : InheritedCliBase {
    [[= mmltk::frameworks::reflection::Minimum<double>{
        0.0}]][[= mmltk::frameworks::reflection::Maximum<double>{1.0}]][[= mmltk::frameworks::reflection::Finite{}]] double derived_ratio = 0.5;
    [[= mmltk::frameworks::reflection::MaxBytes{8U}]] std::string boundary_owned;
};
MMLTK_REFLECT_FIELDS(InheritedCliBase)
MMLTK_REFLECT_FIELDS(InheritedCliRequest)
inline constexpr std::array kInheritedCliOptions{
    mmltk::frameworks::reflection::option<InheritedCliRequest, &InheritedCliBase::inherited_limit>("--inherited-limit", "Inherited bounded scalar",
                                                                                                   "Inherited"),
    mmltk::frameworks::reflection::option<InheritedCliRequest, &InheritedCliRequest::derived_ratio>("--derived-ratio", "Derived bounded scalar", "Inherited"),
};
inline constexpr std::array kInheritedCliExclusions{
    mmltk::frameworks::reflection::unexposed<InheritedCliRequest, &InheritedCliRequest::boundary_owned>("the embedding boundary owns this value"),
};
static_assert((mmltk::frameworks::reflection::audit_descriptors(kInheritedCliOptions, kInheritedCliExclusions), true));
inline constexpr std::array kParserOptions{
    mmltk::frameworks::reflection::option<ParserRequest, mmltk::frameworks::reflection::member_path<&ParserRequest::nested, &ParserNestedState::count>>(
        "--count", "Bounded scalar", "Values", "-c", {}, false, "MMLTK_REFLECTED_CLI_TEST_COUNT"),
    mmltk::frameworks::reflection::option<ParserRequest, &ParserRequest::unsigned_count>("--unsigned-count", "Bounded unsigned scalar", "Values"),
    mmltk::frameworks::reflection::option<ParserRequest, &ParserRequest::ratio>("--ratio", "Bounded finite ratio", "Values"),
    mmltk::frameworks::reflection::option<ParserRequest, &ParserRequest::label>("--label", "Bounded text", "Values"),
    mmltk::frameworks::reflection::option<ParserRequest, &ParserRequest::enabled>("--enabled", "Boolean flag", "Values", {}, "--no-enabled"),
    mmltk::frameworks::reflection::option<ParserRequest, &ParserRequest::optional_path>("--path", "Optional path", "Values"),
    mmltk::frameworks::reflection::option<ParserRequest, &ParserRequest::values>("--value", "Repeatable value", "Values"),
    mmltk::frameworks::reflection::option<ParserRequest, &ParserRequest::mode>("--mode", "Reflected enum", "Values"),
    mmltk::frameworks::reflection::positional<ParserRequest, &ParserRequest::positional>("input", "Positional integer", true),
};
static_assert((mmltk::frameworks::reflection::audit_descriptors(kParserOptions), true));
static_assert(mmltk::frameworks::reflection::reflected_policies_are_valid<ParserRequest>());
static_assert(mmltk::frameworks::reflection::reflected_defaults_are_valid<ParserNestedState>());
static_assert(mmltk::frameworks::reflection::policy_of_member<&ParserRequest::ratio>().finite);
static_assert(mmltk::frameworks::reflection::policy_of_member<&ParserRequest::ratio>().minimum == 0.0L);
static_assert(mmltk::frameworks::reflection::policy_of_member<&ParserRequest::ratio>().maximum == 1.0L);
[[nodiscard]] auto parse_parser_request(const std::span<const std::string_view> arguments) {
    return mmltk::frameworks::reflection::parse<ParserRequest>(arguments, kParserOptions);
}
void test_reflected_cli_inheritance_preserves_identity_order_exclusions_and_presence() {
    std::vector<std::string_view> declaration_order;
    mmltk::frameworks::reflection::visit_materialized_bases<InheritedCliRequest>([&]<class Base>() {
        mmltk::frameworks::reflection::visit_materialized_members<Base>(
            [&]<class Declaration>(const auto& fact) { declaration_order.push_back(fact.member_name); });
    });
    mmltk::frameworks::reflection::visit_materialized_members<InheritedCliRequest>(
        [&]<class Declaration>(const auto& fact) { declaration_order.push_back(fact.member_name); });
    REQUIRE(((declaration_order == std::vector<std::string_view>{"inherited_limit", "derived_ratio", "boundary_owned"})));
    REQUIRE((kInheritedCliOptions[0].name == "--inherited-limit"));
    REQUIRE((kInheritedCliOptions[1].name == "--derived-ratio"));
    constexpr auto inherited_identity =
        mmltk::frameworks::reflection::ReflectedMemberIdentity::from_path<InheritedCliRequest, &InheritedCliBase::inherited_limit>();
    constexpr auto derived_identity =
        mmltk::frameworks::reflection::ReflectedMemberIdentity::from_path<InheritedCliRequest, &InheritedCliRequest::derived_ratio>();
    constexpr auto inherited_index = mmltk::frameworks::reflection::unique_descriptor_index(kInheritedCliOptions, inherited_identity);
    constexpr auto derived_index = mmltk::frameworks::reflection::unique_descriptor_index(kInheritedCliOptions, derived_identity);
    REQUIRE((kInheritedCliOptions[0].terminal_member == inherited_identity));
    REQUIRE((kInheritedCliOptions[0].terminal_member.name() == "inherited_limit"));
    REQUIRE((kInheritedCliOptions[1].terminal_member == derived_identity));
    REQUIRE((kInheritedCliExclusions[0].identity ==
                 mmltk::frameworks::reflection::ReflectedMemberIdentity::from_path<InheritedCliRequest, &InheritedCliRequest::boundary_owned>()));
    const auto absent = mmltk::frameworks::reflection::parse<InheritedCliRequest>({}, kInheritedCliOptions);
    REQUIRE((absent));
    REQUIRE((absent->request.inherited_limit == 4));
    REQUIRE((absent->request.derived_ratio == 0.5));
    REQUIRE((!absent->presence.test(inherited_index)));
    REQUIRE((!absent->presence.test(derived_index)));
    const auto explicit_nondefault =
        mmltk::frameworks::reflection::parse<InheritedCliRequest>(std::array<std::string_view, 2U>{"--inherited-limit", "7"}, kInheritedCliOptions);
    REQUIRE((explicit_nondefault));
    REQUIRE((explicit_nondefault->request.inherited_limit == 7));
    REQUIRE((explicit_nondefault->presence.test(inherited_index)));
    REQUIRE((!explicit_nondefault->presence.test(derived_index)));
    const auto explicit_default =
        mmltk::frameworks::reflection::parse<InheritedCliRequest>(std::array<std::string_view, 2U>{"--inherited-limit", "4"}, kInheritedCliOptions);
    REQUIRE((explicit_default));
    REQUIRE((explicit_default->request.inherited_limit == 4));
    REQUIRE((explicit_default->presence.test(inherited_index)));
    REQUIRE((!explicit_default->presence.test(derived_index)));
}
void test_reflected_cli_policy_boundaries_and_diagnostics() {
    struct Case {
        std::array<std::string_view, 3U> arguments;
        bool accepted;
        mmltk::frameworks::reflection::ParseErrorCode error;
    };
    constexpr std::array cases{
        Case{{"--count", "0", "1"}, true, mmltk::frameworks::reflection::ParseErrorCode::InvalidValue},
        Case{{"--count", "10", "1"}, true, mmltk::frameworks::reflection::ParseErrorCode::InvalidValue},
        Case{{"--count", "-1", "1"}, false, mmltk::frameworks::reflection::ParseErrorCode::InvalidValue},
        Case{{"--count", "11", "1"}, false, mmltk::frameworks::reflection::ParseErrorCode::InvalidValue},
        Case{{"--count", "999999999999999999999", "1"}, false, mmltk::frameworks::reflection::ParseErrorCode::InvalidInteger},
    };
    for (const auto& test : cases) {
        const auto parsed = parse_parser_request(test.arguments);
        REQUIRE((parsed.has_value() == test.accepted));
        if (!test.accepted) REQUIRE((parsed.error().code == test.error));
    }
    struct PolicyCase {
        std::vector<std::string_view> arguments;
        bool accepted;
    };
    const std::string maximum_path(mmltk::frameworks::reflection::kMaximumPathBytes, 'x');
    const std::string oversized_path(mmltk::frameworks::reflection::kMaximumPathBytes + 1U, 'x');
    const std::array policy_cases{
        PolicyCase{{"--unsigned-count", "1", "1"}, true},
        PolicyCase{{"--unsigned-count", "4", "1"}, true},
        PolicyCase{{"--unsigned-count", "0", "1"}, false},
        PolicyCase{{"--unsigned-count", "5", "1"}, false},
        PolicyCase{{"--unsigned-count", "999999999999999999999", "1"}, false},
        PolicyCase{{"--ratio", "0", "1"}, true},
        PolicyCase{{"--ratio", "1", "1"}, true},
        PolicyCase{{"--ratio", "-0.0001", "1"}, false},
        PolicyCase{{"--ratio", "1.0001", "1"}, false},
        PolicyCase{{"--ratio", "nan", "1"}, false},
        PolicyCase{{"--label", "1234", "1"}, true},
        PolicyCase{{"--label", "12345", "1"}, false},
        PolicyCase{{"--path", maximum_path, "1"}, true},
        PolicyCase{{"--path", oversized_path, "1"}, false},
        PolicyCase{{"--value", "1", "--value", "2", "1"}, true},
        PolicyCase{{"--value", "1", "--value", "2", "--value", "3", "1"}, false},
    };
    for (const auto& test : policy_cases) { CHECK(parse_parser_request(std::span<const std::string_view>{test.arguments}).has_value() == test.accepted); }
    struct DirectPolicyCase {
        std::string_view name;
        void (*mutate)(ParserRequest&);
        bool accepted;
    };
    const std::array direct_policy_cases{
        DirectPolicyCase{"signed minimum", [](ParserRequest& value) { value.nested.count = 0; }, true},
        DirectPolicyCase{"signed maximum", [](ParserRequest& value) { value.nested.count = 10; }, true},
        DirectPolicyCase{"signed below", [](ParserRequest& value) { value.nested.count = -1; }, false},
        DirectPolicyCase{"signed above", [](ParserRequest& value) { value.nested.count = 11; }, false},
        DirectPolicyCase{"unsigned minimum", [](ParserRequest& value) { value.unsigned_count = 1U; }, true},
        DirectPolicyCase{"unsigned maximum", [](ParserRequest& value) { value.unsigned_count = 4U; }, true},
        DirectPolicyCase{"unsigned below", [](ParserRequest& value) { value.unsigned_count = 0U; }, false},
        DirectPolicyCase{"unsigned above", [](ParserRequest& value) { value.unsigned_count = 5U; }, false},
        DirectPolicyCase{"unsigned typed overflow", [](ParserRequest& value) { value.unsigned_count = std::numeric_limits<std::uint32_t>::max(); }, false},
        DirectPolicyCase{"float minimum", [](ParserRequest& value) { value.ratio = 0.0; }, true},
        DirectPolicyCase{"float maximum", [](ParserRequest& value) { value.ratio = 1.0; }, true},
        DirectPolicyCase{"float below", [](ParserRequest& value) { value.ratio = -0.0001; }, false},
        DirectPolicyCase{"float above", [](ParserRequest& value) { value.ratio = 1.0001; }, false},
        DirectPolicyCase{"float nonfinite", [](ParserRequest& value) { value.ratio = std::numeric_limits<double>::infinity(); }, false},
        DirectPolicyCase{"text maximum", [](ParserRequest& value) { value.label = "1234"; }, true},
        DirectPolicyCase{"text above", [](ParserRequest& value) { value.label = "12345"; }, false},
        DirectPolicyCase{"path maximum",
                         [](ParserRequest& value) { value.optional_path = fs::path(std::string(mmltk::frameworks::reflection::kMaximumPathBytes, 'x')); },
                         true},
        DirectPolicyCase{"path above",
                         [](ParserRequest& value) { value.optional_path = fs::path(std::string(mmltk::frameworks::reflection::kMaximumPathBytes + 1U, 'x')); },
                         false},
        DirectPolicyCase{"container maximum", [](ParserRequest& value) { value.values = {1, 2}; }, true},
        DirectPolicyCase{"container above", [](ParserRequest& value) { value.values = {1, 2, 3}; }, false},
    };
    for (const auto& test : direct_policy_cases) {
        ParserRequest direct{};
        test.mutate(direct);
        CAPTURE(test.name);
        CHECK(!mmltk::frameworks::reflection::validate_reflected_fields(direct).has_value() == test.accepted);
    }
    const auto unknown = parse_parser_request(std::array<std::string_view, 2U>{"--unknown", "1"});
    REQUIRE((!unknown && unknown.error().code == mmltk::frameworks::reflection::ParseErrorCode::UnknownOption));
    const auto duplicate = parse_parser_request(std::array<std::string_view, 5U>{"--count", "1", "--count", "2", "3"});
    REQUIRE((!duplicate && duplicate.error().code == mmltk::frameworks::reflection::ParseErrorCode::DuplicateOption));
    const auto missing_value = parse_parser_request(std::array<std::string_view, 1U>{"--count"});
    REQUIRE((!missing_value && missing_value.error().code == mmltk::frameworks::reflection::ParseErrorCode::MissingValue));
    const auto missing_positional = parse_parser_request(std::array<std::string_view, 2U>{"--count", "1"});
    REQUIRE((!missing_positional && missing_positional.error().code == mmltk::frameworks::reflection::ParseErrorCode::MissingRequired));
    const auto invalid_enum = parse_parser_request(std::array<std::string_view, 3U>{"--mode", "unknown", "1"});
    REQUIRE((!invalid_enum && invalid_enum.error().code == mmltk::frameworks::reflection::ParseErrorCode::InvalidValue));
    const auto nonfinite = mmltk::frameworks::reflection::parse_scalar<double>("nan");
    REQUIRE((!nonfinite && nonfinite.error().code == mmltk::frameworks::reflection::ParseErrorCode::InvalidFiniteNumber));
}
void test_reflected_cli_optional_repeatable_negation_positionals_environment_and_roundtrip() {
    const auto parsed = parse_parser_request(std::array<std::string_view, 13U>{
        "--no-enabled",
        "--path",
        "/tmp/image.png",
        "--value",
        "4",
        "--value",
        "5",
        "--mode",
        "full-trace",
        "--count=7",
        "--",
        "-9",
        "--not-an-option",
    });
    REQUIRE((!parsed && parsed.error().code == mmltk::frameworks::reflection::ParseErrorCode::UnknownOption));
    const auto valid = parse_parser_request(std::array<std::string_view, 12U>{
        "--no-enabled",
        "--path",
        "/tmp/image.png",
        "--value",
        "4",
        "--value",
        "5",
        "--mode",
        "full",
        "--count=7",
        "--",
        "-9",
    });
    REQUIRE((valid));
    REQUIRE((!valid->request.enabled));
    REQUIRE((valid->request.optional_path == fs::path("/tmp/image.png")));
    REQUIRE(((valid->request.values == std::vector<int>{4, 5})));
    REQUIRE((valid->request.mode == ParserMode::FullTrace));
    REQUIRE((valid->request.nested.count == 7));
    REQUIRE((valid->request.positional == -9));
    std::vector<std::string> emitted;
    mmltk::frameworks::reflection::emit(emitted, valid->request, kParserOptions);
    std::vector<std::string_view> emitted_views;
    emitted_views.reserve(emitted.size());
    for (const auto& token : emitted) emitted_views.emplace_back(token);
    const auto roundtrip = mmltk::frameworks::reflection::parse<ParserRequest>(emitted_views, kParserOptions);
    REQUIRE((roundtrip));
    REQUIRE((roundtrip->request == valid->request));
    const auto capacity = parse_parser_request(std::array<std::string_view, 8U>{
        "--value",
        "1",
        "--value",
        "2",
        "--value",
        "3",
        "--",
        "0",
    });
    REQUIRE((!capacity && capacity.error().code == mmltk::frameworks::reflection::ParseErrorCode::InvalidValue));
    REQUIRE((::setenv("MMLTK_REFLECTED_CLI_TEST_COUNT", "6", 1) == 0));
    const auto environment = parse_parser_request(std::array<std::string_view, 1U>{"2"});
    const auto command_override = parse_parser_request(std::array<std::string_view, 3U>{"--count", "8", "2"});
    REQUIRE((::unsetenv("MMLTK_REFLECTED_CLI_TEST_COUNT") == 0));
    REQUIRE((environment && environment->request.nested.count == 6));
    REQUIRE((command_override && command_override->request.nested.count == 8));
    const auto empty_optional = parse_parser_request(std::array<std::string_view, 3U>{"--path=", "--", "1"});
    REQUIRE((empty_optional && !empty_optional->request.optional_path.has_value()));
    const std::string help = mmltk::frameworks::reflection::help("test [options]", "Descriptor help", kParserOptions);
    for (const auto& option : kParserOptions) REQUIRE((help.find(option.name) != std::string::npos));
}
void test_reflected_cli_scalar_success_has_no_parser_owned_allocation() {
    constexpr std::array<std::string_view, 6U> arguments{
        "--count", "4", "--enabled", "--mode", "fast", "1",
    };
    g_cli_allocation_count = 0U;
    g_count_cli_allocations = true;
    const auto parsed = parse_parser_request(arguments);
    g_count_cli_allocations = false;
    REQUIRE((parsed));
    REQUIRE((g_cli_allocation_count == 0U));
}
void test_subprocess_capture_keeps_stdout_and_stderr_separate() {
    const SubprocessResult result = run_subprocess_capture_output({
        "/bin/sh",
        "-c",
        "printf 'stdout-line\\n'; printf 'stderr-line\\n' >&2",
    });
    REQUIRE((result.exit_code == 0));
    REQUIRE((result.stdout_text == "stdout-line\n"));
    REQUIRE((result.stderr_text == "stderr-line\n"));
    REQUIRE((result.output_text.find("stdout-line\n") != std::string::npos));
    REQUIRE((result.output_text.find("stderr-line\n") != std::string::npos));
}
void test_rfdetr_info_forwards_explicit_logging_options() {
    const ScopedTempDir root("mmltk_rfdetr_info_logging");
    const auto missing = root.path() / "missing.onnx";
    const auto file = root.path() / "explicit.log";
    const auto directory = root.path() / "directory";
    struct LoggingCase {
        std::vector<std::string> environment;
        std::vector<std::string> options;
        fs::path sink;
        bool enabled = false;
    };
    const std::vector<LoggingCase> cases{
        {{}, {}, {}, false},
        {{}, {"--log-level=off", "--log-file=" + file.string()}, file, false},
        {{"MMLTK_LOG_LEVEL=debug"}, {"--log-level", "off", "--log-file", file.string()}, file, false},
        {{"MMLTK_LOG_LEVEL=off"}, {"--log-level", "info", "--log-file", file.string()}, file, true},
        {{}, {"--log-file=" + file.string()}, file, true},
        {{}, {"--log-level=off", "--log-level=info", "--log-dir=" + directory.string()}, directory / "mmltk-rfdetr-onnx-info.log", true},
        {{"MMLTK_LOG_LEVEL=off"}, {"--log-level=info", "--log-level=", "--log-file=" + file.string()}, file, false},
    };
    for (const auto& entry : cases) {
        fs::remove(file);
        fs::remove_all(directory);
        std::vector<std::string> command{"env", "-u", "MMLTK_LOG_LEVEL", "-u", "MMLTK_LOG_FILE", "-u", "MMLTK_LOG_DIR"};
        command.insert(command.end(), entry.environment.begin(), entry.environment.end());
        command.push_back(mmltk_cli_path());
        command.insert(command.end(), entry.options.begin(), entry.options.end());
        command.insert(command.end(), {"rfdetr", "info", "--onnx", missing.string()});
        const auto result = run_subprocess_capture_output(command);
        INFO("exit status=" << result.exit_code << "\n" << result.output_text);
        CHECK(result.exit_code == 1);
        CHECK(result.stdout_text.empty());
        if (!entry.enabled) {
            CHECK(result.stderr_text.empty());
            if (!entry.sink.empty()) CHECK_FALSE(fs::exists(entry.sink));
        } else {
            REQUIRE(fs::exists(entry.sink));
            const auto lines = read_text_lines(entry.sink);
            assert_contains_substring(lines, "mmltk rfdetr onnx info error:");
            assert_contains_substring(lines, "missing.onnx");
        }
    }
}
void test_log_file_flag_creates_requested_log_file() {
    const fs::path temp_dir = make_temp_dir("mmltk-log-file-flag");
    const fs::path log_path = temp_dir / "explicit.log";
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--log-file",
        log_path.string(),
        "--help",
    });
    REQUIRE((result.exit_code == 0));
    REQUIRE((fs::exists(log_path)));
    fs::remove(log_path);
    for (const std::string level_source : {"MMLTK_LOG_LEVEL=off", "MMLTK_LOG_LEVEL=debug"}) {
        const SubprocessResult disabled = run_subprocess_capture_output({
            "env",
            level_source,
            mmltk_cli_path(),
            "--log-level=off",
            "--log-file",
            log_path.string(),
            "--help",
        });
        REQUIRE((disabled.exit_code == 0));
        REQUIRE((disabled.stderr_text.empty()));
        REQUIRE((!fs::exists(log_path)));
    }
    cleanup_temp_dir(temp_dir);
}
void test_log_dir_flag_creates_default_log_file() {
    const fs::path temp_dir = make_temp_dir("mmltk-log-dir-flag");
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--log-dir",
        temp_dir.string(),
        "--help",
    });
    REQUIRE((result.exit_code == 0));
    REQUIRE((fs::exists(temp_dir / "mmltk.log")));
    cleanup_temp_dir(temp_dir);
}
void test_env_log_file_creates_requested_log_file() {
    const fs::path temp_dir = make_temp_dir("mmltk-log-file-env");
    const fs::path log_path = temp_dir / "env.log";
    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "MMLTK_LOG_FILE=" + log_path.string(),
        mmltk_cli_path(),
        "--help",
    });
    REQUIRE((result.exit_code == 0));
    REQUIRE((fs::exists(log_path)));
    fs::remove(log_path);
    for (const bool override_off : {false, true}) {
        std::vector<std::string> arguments{
            "env", "MMLTK_LOG_LEVEL=off", "MMLTK_LOG_FILE=" + log_path.string(), mmltk_cli_path(), "--help",
        };
        if (override_off) arguments.push_back("--log-level=info");
        const SubprocessResult overridden = run_subprocess_capture_output(arguments);
        REQUIRE((overridden.exit_code == 0));
        REQUIRE((fs::exists(log_path) == override_off));
    }
    cleanup_temp_dir(temp_dir);
}
void test_env_log_dir_creates_default_log_file() {
    const fs::path temp_dir = make_temp_dir("mmltk-log-dir-env");
    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "MMLTK_LOG_DIR=" + temp_dir.string(),
        mmltk_cli_path(),
        "--help",
    });
    REQUIRE((result.exit_code == 0));
    REQUIRE((fs::exists(temp_dir / "mmltk.log")));
    cleanup_temp_dir(temp_dir);
}
void test_invalid_log_level_flag_reports_error_on_stderr() {
    const SubprocessResult result = run_subprocess_capture_output({
        mmltk_cli_path(),
        "--log-level",
        "banana",
        "--help",
    });
    REQUIRE((result.exit_code == 1));
    REQUIRE((result.stderr_text.find("invalid MMLTK log level: banana") != std::string::npos));
}
void test_invalid_log_level_env_reports_error_on_stderr() {
    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "MMLTK_LOG_LEVEL=banana",
        mmltk_cli_path(),
        "--help",
    });
    REQUIRE((result.exit_code == 1));
    REQUIRE((result.stderr_text.find("invalid MMLTK log level: banana") != std::string::npos));
}
void prepare_fake_docker_state(const fs::path& temp_dir, const fs::path& state_dir) {
    std::error_code error;
    fs::create_directories(state_dir, error);
    REQUIRE((!error));
    write_fake_docker_script(temp_dir);
    const fs::path wrapper = locate_repo_wrapper_path();
    fs::create_directories(temp_dir / "tools");
    fs::copy_file(wrapper, temp_dir / "mmltk", fs::copy_options::overwrite_existing);
    fs::copy_file(wrapper.parent_path() / "tools/runtime_package.sh", temp_dir / "tools/runtime_package.sh", fs::copy_options::overwrite_existing);
}
std::vector<std::string> capture_wrapper_logging_arguments(const fs::path& temp_dir, const fs::path& state_dir, const std::vector<std::string>& environment,
                                                           const std::vector<std::string>& arguments) {
    fs::remove(state_dir / "exec_count.txt");
    fs::remove_all(temp_dir / ".mmltk-data");
    std::vector<std::string> command{
        "env",
        "-u",
        "MMLTK_LOG_LEVEL",
        "-u",
        "MMLTK_LOG_FILE",
        "-u",
        "MMLTK_LOG_DIR",
        "-u",
        "MMLTK_CACHE_ROOT",
        "-u",
        "MMLTK_RELEASE_STAGE_ROOT",
        "PATH=" + prepend_path_env(temp_dir / "bin"),
        "MMLTK_FAKE_DOCKER_STATE=" + state_dir.string(),
        "MMLTK_IMAGE=fake-mmltk",
    };
    command.insert(command.end(), environment.begin(), environment.end());
    command.push_back((temp_dir / "mmltk").string());
    command.insert(command.end(), arguments.begin(), arguments.end());
    const SubprocessResult result = run_subprocess_capture_output(command);
    INFO("wrapper stdout:\n" << result.stdout_text << "\nwrapper stderr:\n" << result.stderr_text);
    REQUIRE((result.exit_code == 0));
    return read_text_lines(state_dir / "exec_1_args.txt");
}
void test_wrapper_env_logging_overrides_are_forwarded_to_docker_exec() {
    const fs::path temp_dir = make_temp_dir("mmltk-wrapper-log-env");
    const fs::path state_dir = temp_dir / "state";
    const fs::path log_path = temp_dir / "wrapper.log";
    const fs::path log_dir = temp_dir / "logs";
    prepare_fake_docker_state(temp_dir, state_dir);
    const auto exec_args = capture_wrapper_logging_arguments(
        temp_dir, state_dir, {"MMLTK_LOG_LEVEL=debug", "MMLTK_LOG_FILE=" + log_path.string(), "MMLTK_LOG_DIR=" + log_dir.string()}, {"--help"});
    assert_contains_line(exec_args, "--env");
    assert_contains_line(exec_args, "MMLTK_LOG_LEVEL=debug");
    assert_contains_line(exec_args, "MMLTK_LOG_FILE=/host" + log_path.string());
    assert_contains_line(exec_args, "MMLTK_LOG_DIR=/host" + log_dir.string());
    assert_contains_line(exec_args, "/opt/mmltk/bin/mmltk");
    assert_contains_line(exec_args, "--help");
    REQUIRE((!fs::exists(temp_dir / ".mmltk-data/logs")));
    const std::string fallback = "MMLTK_LOG_DIR=/host" + (temp_dir / ".mmltk-data/logs").string();
    for (const std::string level : {"", "info", "off", "OFF"}) {
        const auto arguments = capture_wrapper_logging_arguments(
            temp_dir, state_dir, level.empty() ? std::vector<std::string>{} : std::vector<std::string>{"MMLTK_LOG_LEVEL=" + level}, {"--help"});
        REQUIRE(((std::find(arguments.begin(), arguments.end(), fallback) != arguments.end()) == (level == "info")));
        REQUIRE((fs::exists(temp_dir / ".mmltk-data/logs") == (level == "info")));
        if (level.empty()) {
            REQUIRE((std::none_of(arguments.begin(), arguments.end(), [](const auto& argument) { return argument.starts_with("MMLTK_LOG_"); })));
        } else {
            assert_contains_line(arguments, "MMLTK_LOG_LEVEL=" + level);
        }
    }
    cleanup_temp_dir(temp_dir);
}
void test_wrapper_cli_logging_flags_are_forwarded_to_container_command() {
    const fs::path temp_dir = make_temp_dir("mmltk-wrapper-log-cli");
    const fs::path state_dir = temp_dir / "state";
    const fs::path log_path = temp_dir / "explicit.log";
    const fs::path log_dir = temp_dir / "logdir";
    prepare_fake_docker_state(temp_dir, state_dir);
    const auto exec_args = capture_wrapper_logging_arguments(
        temp_dir, state_dir, {}, {"--log-level", "trace", "--log-file", log_path.string(), "--log-dir", log_dir.string(), "--help"});
    assert_contains_line(exec_args, "/opt/mmltk/bin/mmltk");
    assert_contains_line(exec_args, "--log-level");
    assert_contains_line(exec_args, "trace");
    assert_contains_line(exec_args, "--log-file");
    assert_contains_line(exec_args, "/host" + log_path.string());
    assert_contains_line(exec_args, "--log-dir");
    assert_contains_line(exec_args, "/host" + log_dir.string());
    assert_contains_line(exec_args, "--help");
    struct LoggingCase {
        std::vector<std::string> environment;
        std::vector<std::string> arguments;
        bool fallback;
        bool invalid_level = false;
    };
    const std::vector<LoggingCase> cases{
        {{}, {"--log-level", "info"}, true},
        {{}, {"--log-level=debug"}, true},
        {{}, {"--log-level=off"}, false},
        {{"MMLTK_LOG_LEVEL=off"}, {"--log-level=info"}, true},
        {{"MMLTK_LOG_LEVEL=debug"}, {"--log-level=off"}, false},
        {{"MMLTK_LOG_LEVEL=off"}, {"--log-level=info", "--log-level="}, false},
        {{"MMLTK_LOG_LEVEL=info"}, {"--log-level=off", "--log-level="}, true},
        {{"MMLTK_LOG_LEVEL=info"}, {"--log-file", log_path.string()}, false},
        {{"MMLTK_LOG_LEVEL=info"}, {"--log-dir=" + log_dir.string()}, false},
        {{"MMLTK_LOG_FILE=" + log_path.string()}, {"--log-level=info"}, false},
        {{"MMLTK_LOG_DIR=" + log_dir.string()}, {"--log-level=info"}, false},
        {{}, {"--log-level=banana"}, false, true},
        {{"MMLTK_LOG_LEVEL=banana"}, {"--log-level=info"}, false, true},
        {{}, {"--log-level=banana", "--log-level=info"}, false, true},
        {{}, {"--log-level"}, false},
    };
    const std::string fallback = "MMLTK_LOG_DIR=/host" + (temp_dir / ".mmltk-data/logs").string();
    for (const auto& logging_case : cases) {
        const auto arguments = capture_wrapper_logging_arguments(temp_dir, state_dir, logging_case.environment, logging_case.arguments);
        REQUIRE(((std::find(arguments.begin(), arguments.end(), fallback) != arguments.end()) == logging_case.fallback));
        REQUIRE((fs::exists(temp_dir / ".mmltk-data/logs") == logging_case.fallback));
        if (logging_case.invalid_level) {
            REQUIRE((std::none_of(arguments.begin(), arguments.end(), [](const auto& argument) { return argument.starts_with("MMLTK_LOG_DIR="); })));
            for (const auto& entry : logging_case.environment) assert_contains_line(arguments, entry);
            REQUIRE((arguments.size() >= logging_case.arguments.size()));
            REQUIRE((std::equal(logging_case.arguments.rbegin(), logging_case.arguments.rend(), arguments.rbegin())));
        }
    }
    cleanup_temp_dir(temp_dir);
}
void test_wrapper_gui_tmpfs_uses_target_uid_gid() {
    const fs::path temp_dir = make_temp_dir("mmltk-wrapper-gui-tmpfs");
    const fs::path state_dir = temp_dir / "state";
    const fs::path runtime_dir = temp_dir / "runtime";
    const fs::path wayland_socket_path = runtime_dir / "wayland-0";
    std::error_code error;
    fs::create_directories(state_dir, error);
    REQUIRE((!error));
    fs::create_directories(runtime_dir, error);
    REQUIRE((!error));
    {
        std::ofstream stream(wayland_socket_path, std::ios::trunc);
        REQUIRE((stream.is_open()));
        stream << "fake-wayland-socket";
    }
    write_fake_docker_script(temp_dir);
    const SubprocessResult result = run_subprocess_capture_output({
        "env",
        "PATH=" + prepend_path_env(temp_dir / "bin"),
        "MMLTK_FAKE_DOCKER_STATE=" + state_dir.string(),
        "MMLTK_IMAGE=fake-mmltk",
        "MMLTK_USER=root",
        "WAYLAND_DISPLAY=wayland-0",
        "XDG_RUNTIME_DIR=" + runtime_dir.string(),
        locate_repo_wrapper_path().string(),
        "--prepare-gui-container",
    });
    INFO("wrapper stdout:\n" << result.stdout_text << "\nwrapper stderr:\n" << result.stderr_text);
    REQUIRE((result.exit_code == 0));
    const std::vector<std::string> run_args = read_text_lines(state_dir / "run_args.txt");
    const std::string expected_tmpfs = "/tmp/mmltk-gui-runtime:rw,mode=700,uid=" + std::to_string(::getuid()) + ",gid=" + std::to_string(::getgid());
    assert_contains_line(run_args, "--tmpfs");
    assert_contains_line(run_args, expected_tmpfs);
    assert_contains_substring(run_args, "com.mmltk.runtime=");
    assert_contains_substring(run_args, "tmpfs=" + expected_tmpfs);
    cleanup_temp_dir(temp_dir);
}
}  // namespace
TEST_CASE("test_subprocess_capture_keeps_stdout_and_stderr_separate", "[core][cli][subprocess]") { test_subprocess_capture_keeps_stdout_and_stderr_separate(); }
TEST_CASE("test_reflected_cli_inheritance_preserves_identity_order_exclusions_and_presence", "[core][cli][reflected][inheritance]") { test_reflected_cli_inheritance_preserves_identity_order_exclusions_and_presence(); }
TEST_CASE("test_reflected_cli_policy_boundaries_and_diagnostics", "[core][cli][reflected]") { test_reflected_cli_policy_boundaries_and_diagnostics(); }
TEST_CASE("test_reflected_cli_optional_repeatable_negation_positionals_environment_and_roundtrip", "[core][cli][reflected]") { test_reflected_cli_optional_repeatable_negation_positionals_environment_and_roundtrip(); }
TEST_CASE("test_reflected_cli_scalar_success_has_no_parser_owned_allocation", "[core][cli][reflected][allocation]") { test_reflected_cli_scalar_success_has_no_parser_owned_allocation(); }
TEST_CASE("test_log_file_flag_creates_requested_log_file", "[core][cli][logging]") { test_log_file_flag_creates_requested_log_file(); }
TEST_CASE("test_log_dir_flag_creates_default_log_file", "[core][cli][logging]") { test_log_dir_flag_creates_default_log_file(); }
TEST_CASE("test_env_log_file_creates_requested_log_file", "[core][cli][logging]") { test_env_log_file_creates_requested_log_file(); }
TEST_CASE("test_env_log_dir_creates_default_log_file", "[core][cli][logging]") { test_env_log_dir_creates_default_log_file(); }
TEST_CASE("test_invalid_log_level_flag_reports_error_on_stderr", "[core][cli][logging]") { test_invalid_log_level_flag_reports_error_on_stderr(); }
TEST_CASE("test_invalid_log_level_env_reports_error_on_stderr", "[core][cli][logging]") { test_invalid_log_level_env_reports_error_on_stderr(); }
TEST_CASE("test_wrapper_env_logging_overrides_are_forwarded_to_docker_exec", "[core][cli][logging][wrapper]") { test_wrapper_env_logging_overrides_are_forwarded_to_docker_exec(); }
TEST_CASE("test_wrapper_cli_logging_flags_are_forwarded_to_container_command", "[core][cli][logging][wrapper]") { test_wrapper_cli_logging_flags_are_forwarded_to_container_command(); }
TEST_CASE("test_wrapper_gui_tmpfs_uses_target_uid_gid", "[core][cli][wrapper][gui]") { test_wrapper_gui_tmpfs_uses_target_uid_gid(); }
TEST_CASE("negative reflected flags preserve canonical defaults and round trip polarity", "[cli][reflection]") {
    namespace reflection = mmltk::frameworks::reflection;
    constexpr std::array descriptors{reflection::negative_flag<ParserRequest, &ParserRequest::enabled>("--disable", "Disable the canonical boolean")};
    for (const bool enabled : {false, true}) {
        ParserRequest request{};
        request.enabled = enabled;
        std::vector<std::string> emitted;
        reflection::emit(emitted, request, descriptors);
        CHECK(emitted.size() == (enabled ? 0U : 1U));
        std::vector<std::string_view> arguments{emitted.begin(), emitted.end()};
        const auto parsed = reflection::parse<ParserRequest>(arguments, descriptors);
        REQUIRE(parsed);
        CHECK(parsed->request.enabled == enabled);
    }
    const std::array<std::string_view, 1> false_flag{"--disable=false"};
    const auto parsed = reflection::parse<ParserRequest>(false_flag, descriptors);
    REQUIRE(parsed);
    CHECK(parsed->request.enabled);
}
TEST_CASE("test_rfdetr_info_forwards_explicit_logging_options", "[core][cli][logging][rfdetr]") { test_rfdetr_info_forwards_explicit_logging_options(); }
TEST_CASE("RF-DETR help exposes independent augmentation and compiler resampling controls", "[core][cli][rfdetr][perceptual]") {
    for (const auto command : {"train", "compile"}) {
        const auto result = run_subprocess_capture_output({mmltk_cli_path(), "rfdetr", command, "--help"});
        REQUIRE(result.exit_code == 0);
        if (std::string_view(command) == "train") {
            CHECK(result.stdout_text.find("--gpu-augment") != std::string::npos);
            CHECK(result.stdout_text.find("--aug-perceptual-downscale") != std::string::npos);
        } else {
            CHECK(result.stdout_text.find("--perceptual-downscale") != std::string::npos);
        }
    }
}
