module;
#include <algorithm>
#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <meta>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>
export module mmltk.tools.tool_command;
import mmltk.tools.cpp_support;
export namespace mmltk::tools::command {
// External spelling is projected from the enum identifier. The annotation
// preserves the logical program name in existing help and fixture protocols.
struct ProgramName {
 const char* value;
 consteval explicit ProgramName(std::string_view name) : value(std::define_static_string(name)) {}
};
enum class Command {
 audit_includes[[= ProgramName{"audit_includes.mjs"}]],
 audit_test_inventory[[= ProgramName{"audit_test_inventory.py"}]],
 build_time_trace[[= ProgramName{"build_time_trace.sh"}]],
 check_cargo_dupes[[= ProgramName{"check_cargo_dupes.sh"}]],
 check_toolchain_invariants[[= ProgramName{"check_toolchain_invariants.py"}]],
 collect_time_traces[[= ProgramName{"collect_time_traces.sh"}]],
 configure_cmake_graph[[= ProgramName{"configure_cmake_graph.sh"}]],
 diagnose_benchmark_image[[= ProgramName{"diagnose_benchmark_image.py"}]],
 diagnose_dataset_io[[= ProgramName{"diagnose_dataset_io.py"}]],
 diagnose_gpu_environment[[= ProgramName{"diagnose_gpu_environment.py"}]],
 diagnose_native_link[[= ProgramName{"diagnose_native_link.py"}]],
 diagnose_native_process[[= ProgramName{"diagnose_native_process.py"}]],
 diagnose_native_symbols[[= ProgramName{"diagnose_native_symbols.py"}]],
 diagnose_nvidia_payload[[= ProgramName{"diagnose_nvidia_payload.py"}]],
 format_declarations[[= ProgramName{"format_declarations.mjs"}]],
 generate_cleanup_json[[= ProgramName{"generate_cleanup_json.mjs"}]],
 headless_compositor[[= ProgramName{"headless_compositor"}]],
 log_query[[= ProgramName{"log_query.py"}]],
 module_safe_compiler_launcher[[= ProgramName{"module_safe_compiler_launcher.sh"}]],
 run_codex_actionplan[[= ProgramName{"run_codex_actionplan.sh"}]],
 run_static_analysis[[= ProgramName{"run_static_analysis.sh"}]],
 run_traced_command[[= ProgramName{"run_traced_command.sh"}]],
 run_uncomment[[= ProgramName{"run_uncomment.sh"}]],
 runtime_package[[= ProgramName{"runtime_package"}]],
 test_audit_includes[[= ProgramName{"audit_includes.test"}]],
 test_format_declarations[[= ProgramName{"format_declarations.test"}]],
 test_generate_cleanup_json[[= ProgramName{"generate_cleanup_json.test"}]],
 test_review_candidates[[= ProgramName{"review_candidates.test"}]],
 test_headless_compositor[[= ProgramName{"test_headless_compositor"}]],
 test_log_query[[= ProgramName{"test_log_query"}]],
};
struct Descriptor {
 Command command;
 std::string_view name, program;
};
}  // namespace mmltk::tools::command
namespace mmltk::tools::command {
consteval auto descriptors() {
 static constexpr auto values = std::define_static_array(std::meta::enumerators_of(^^Command));
 std::array<Descriptor, values.size()> result{};
 std::size_t index = 0;
 template for (constexpr auto value : values) {
  constexpr auto annotations = std::define_static_array(std::meta::annotations_of(value));
  static_assert(annotations.size() == 1);
  constexpr auto program = std::meta::extract<ProgramName>(annotations[0]);
  auto spelling = std::string(std::meta::identifier_of(value));
  std::replace(spelling.begin(), spelling.end(), '_', '-');
  result[index++] = {[:value:], std::define_static_string(spelling), program.value};
 }
 return result;
}
}  // namespace mmltk::tools::command
export namespace mmltk::tools::command {
inline constexpr auto commands = descriptors();
constexpr std::string_view name(Command selected) { return commands.at(static_cast<std::size_t>(selected)).name; }
constexpr bool fixture(Command selected) { return name(selected).starts_with("test-"); }
constexpr std::string_view program(Command selected) { return commands.at(static_cast<std::size_t>(selected)).program; }
inline std::optional<Command> parse(std::string_view spelling) {
 for (const auto& item : commands)
  if (item.name == spelling) return item.command;
 return std::nullopt;
}
// Expanding the canonical descriptors makes a missing implementation a compile
// error in dispatch; importers do not re-reflect the command vocabulary.
template <class Visitor>
int visit(Command selected, Visitor&& visitor) {
 template for (constexpr auto descriptor : commands) {
  if (selected == descriptor.command) return visitor(std::integral_constant<Command, descriptor.command>{});
 }
 throw ToolError("invalid native tool command", 2);
}
inline std::vector<std::string> self_invocation(Command selected, const std::vector<std::string>& arguments = {}, std::string_view argv0 = {}, std::string_view executable = {}) {
 std::vector<std::string> result{executable.empty() ? executable_path().string() : std::string(executable), "--tool", std::string(name(selected))};
 if (!argv0.empty()) {
  result.emplace_back("--argv0");
  result.emplace_back(argv0);
 }
 result.emplace_back("--");
 result.insert(result.end(), arguments.begin(), arguments.end());
 return result;
}
inline std::string shell_invocation(Command selected, const std::vector<std::string>& arguments = {}, std::string_view argv0 = {}, std::string_view executable = {}) {
 std::string result;
 for (const auto& argument : self_invocation(selected, arguments, argv0, executable)) {
  if (!result.empty()) result += ' ';
  result += shell_quote(argument);
 }
 return result;
}
// Arguments exclude the logical argv[0]. This boundary creates mutable argv
// storage because compiler launchers deliberately replace argv[0] before exec.
// Entrypoints retain their process role (including exec and signal setup).
// Use self_invocation for resource-owning commands; safe owner APIs can be
// imported and called directly without entering a process entrypoint.
int invoke(Command selected, const std::vector<std::string>& arguments = {}, std::string_view argv0 = {});
// argc/argv start at command selection, after the wrapper's --tool argument.
int entry(int argc, char** argv);
}  // namespace mmltk::tools::command
