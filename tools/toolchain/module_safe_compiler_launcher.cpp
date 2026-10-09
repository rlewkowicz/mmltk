module;
#include <cerrno>
#include <cstdio>
#include <unistd.h>
export module mmltk.tools.module_safe_compiler_launcher;
import std;
export int mmltk_tool_module_safe_compiler_launcher(int argc, char* argv[]) {
 if (argc < 2) {
  std::fputs("mmltk compiler launcher: missing compiler command\n", stderr);
  return 2;
 }
 bool uses_modules = false;
 for (int index = 2; index < argc; ++index) {
  const std::string_view argument{argv[index]};
  if (argument == "-fmodules" || argument.starts_with("-fmodules=") || argument == "-fmodules-ts" || argument.starts_with("-fmodule-mapper=")) {
   uses_modules = true;
   break;
  }
 }
 // Reuse the original, null-terminated argument array without copying arguments.
 char ccache[] = "ccache";
 char** command = argv + 1;
 if (!uses_modules) {
  argv[0] = ccache;
  command = argv;
 }
 ::execvp(command[0], command);
 const int error = errno;
 std::fprintf(stderr, "mmltk compiler launcher: %s: %s\n", command[0], std::strerror(error));
 return error == ENOENT ? 127 : 126;
}
