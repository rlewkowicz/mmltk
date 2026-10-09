export module mmltk.tools.tool_dispatch;
import std;
import mmltk.tools.tool_command;
import mmltk.tools.cpp_support;
import mmltk.tools.audit_includes;
import mmltk.tools.audit_test_inventory;
import mmltk.tools.build_time_trace;
import mmltk.tools.check_cargo_dupes;
import mmltk.tools.check_toolchain_invariants;
import mmltk.tools.collect_time_traces;
import mmltk.tools.configure_cmake_graph;
import mmltk.tools.diagnose_benchmark_image;
import mmltk.tools.diagnose_dataset_io;
import mmltk.tools.diagnose_gpu_environment;
import mmltk.tools.diagnose_native_link;
import mmltk.tools.diagnose_native_process;
import mmltk.tools.diagnose_native_symbols;
import mmltk.tools.diagnose_nvidia_payload;
import mmltk.tools.format_declarations;
import mmltk.tools.generate_cleanup_json;
import mmltk.tools.headless_compositor;
import mmltk.tools.log_query;
import mmltk.tools.module_safe_compiler_launcher;
import mmltk.tools.run_codex_actionplan;
import mmltk.tools.run_static_analysis;
import mmltk.tools.run_traced_command;
import mmltk.tools.run_uncomment;
import mmltk.tools.runtime_package;
namespace mmltk::tools::dispatch {
namespace {
using command::Command;
template <Command Selected>
constexpr auto implementation() {
 if constexpr (Selected == Command::audit_includes)
  return &mmltk_tool_audit_includes;
 else if constexpr (Selected == Command::audit_test_inventory)
  return &mmltk_tool_audit_test_inventory;
 else if constexpr (Selected == Command::build_time_trace)
  return &mmltk_tool_build_time_trace;
 else if constexpr (Selected == Command::check_cargo_dupes)
  return &mmltk_tool_check_cargo_dupes;
 else if constexpr (Selected == Command::check_toolchain_invariants)
  return &mmltk_tool_check_toolchain_invariants;
 else if constexpr (Selected == Command::collect_time_traces)
  return &mmltk_tool_collect_time_traces;
 else if constexpr (Selected == Command::configure_cmake_graph)
  return &mmltk_tool_configure_cmake_graph;
 else if constexpr (Selected == Command::diagnose_benchmark_image)
  return &mmltk_tool_diagnose_benchmark_image;
 else if constexpr (Selected == Command::diagnose_dataset_io)
  return &mmltk_tool_diagnose_dataset_io;
 else if constexpr (Selected == Command::diagnose_gpu_environment)
  return &mmltk_tool_diagnose_gpu_environment;
 else if constexpr (Selected == Command::diagnose_native_link)
  return &mmltk_tool_diagnose_native_link;
 else if constexpr (Selected == Command::diagnose_native_process)
  return &mmltk_tool_diagnose_native_process;
 else if constexpr (Selected == Command::diagnose_native_symbols)
  return &mmltk_tool_diagnose_native_symbols;
 else if constexpr (Selected == Command::diagnose_nvidia_payload)
  return &mmltk_tool_diagnose_nvidia_payload;
 else if constexpr (Selected == Command::format_declarations)
  return &mmltk_tool_format_declarations;
 else if constexpr (Selected == Command::generate_cleanup_json)
  return &mmltk_tool_generate_cleanup_json;
 else if constexpr (Selected == Command::headless_compositor)
  return &mmltk_tool_headless_compositor;
 else if constexpr (Selected == Command::log_query)
  return &mmltk::log_query::entry;
 else if constexpr (Selected == Command::module_safe_compiler_launcher)
  return &mmltk_tool_module_safe_compiler_launcher;
 else if constexpr (Selected == Command::run_codex_actionplan)
  return &mmltk_tool_run_codex_actionplan;
 else if constexpr (Selected == Command::run_static_analysis)
  return &mmltk_tool_run_static_analysis;
 else if constexpr (Selected == Command::run_traced_command)
  return &mmltk_tool_run_traced_command;
 else if constexpr (Selected == Command::run_uncomment)
  return &mmltk_tool_run_uncomment;
 else if constexpr (Selected == Command::runtime_package)
  return &mmltk_tool_runtime_package;
 else
  static_assert(false, "native command has no implementation");
}
}  // namespace
}  // namespace mmltk::tools::dispatch
export namespace mmltk::tools::dispatch {
int invoke(const command::Invocation& invocation) {
 return command::visit(invocation.selected, [&](auto selected) -> int {
  if constexpr (command::fixture(decltype(selected)::value)) {
   auto runner = executable_path().parent_path() / "mmltk-tests";
   if (!fs::exists(runner)) runner = executable_path().parent_path() / "build/tools/mmltk-tests";
   tools::exec(command::self_invocation(invocation.selected, invocation.arguments, invocation.logical_name.value_or(""), runner.string()));
  } else
   return invocation.call(implementation<decltype(selected)::value>());
 });
}
int entry(int argc, char** argv) {
 return command::run_entry(argc, argv, invoke);
}
}  // namespace mmltk::tools::dispatch
