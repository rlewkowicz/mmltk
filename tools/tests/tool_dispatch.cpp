export module mmltk.tools.tests.tool_dispatch;
import std;
import mmltk.tools.tool_command;
import mmltk.tools.tool_dispatch;
import mmltk.tools.log_query;
import mmltk.tools.tests.audit_includes_test;
import mmltk.tools.tests.format_declarations_test;
import mmltk.tools.tests.generate_cleanup_json_test;
import mmltk.tools.tests.review_candidates_test;
import mmltk.tools.tests.test_headless_compositor;
namespace mmltk::tools::tests::dispatch {
namespace {
using command::Command;
template <Command Selected>
constexpr auto implementation() {
 if constexpr (Selected == Command::test_audit_includes)
  return &mmltk_tool_audit_includes_test;
 else if constexpr (Selected == Command::test_format_declarations)
  return &mmltk_tool_format_declarations_test;
 else if constexpr (Selected == Command::test_generate_cleanup_json)
  return &mmltk_tool_generate_cleanup_json_test;
 else if constexpr (Selected == Command::test_review_candidates)
  return &mmltk_tool_review_candidates_test;
 else if constexpr (Selected == Command::test_headless_compositor)
  return &mmltk_tool_test_headless_compositor;
 else if constexpr (Selected == Command::test_log_query)
  return &mmltk::log_query::test_entry;
 else
  static_assert(false, "native fixture command has no implementation");
}
}  // namespace
}  // namespace mmltk::tools::tests::dispatch
export namespace mmltk::tools::tests::dispatch {
int invoke(const command::Invocation& invocation) {
 return command::visit(invocation.selected, [&](auto selected) -> int {
  if constexpr (command::fixture(decltype(selected)::value))
   return invocation.call(implementation<decltype(selected)::value>());
  else
   return tools::dispatch::invoke(invocation);
 });
}
int entry(int argc, char** argv) { return command::run_entry(argc, argv, invoke); }
}  // namespace mmltk::tools::tests::dispatch
