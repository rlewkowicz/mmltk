// Native counterpart of collect_time_traces.sh. Requires cpp_support.hpp,
// the vendored nlohmann JSON headers, and GNU realpath on PATH.
export module mmltk.tools.collect_time_traces;
import std;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
namespace {
namespace fs = std::filesystem;
namespace support = mmltk::tools;
fs::path canonical_missing(const char* argument) {
 // GNU -m also handles nonexistent/non-directory components and symlink
 // loops; weakly_canonical does not implement the same contract. Command
 // substitution removes all trailing newlines in the original script.
 const auto result = support::capture({"realpath", "-m", argument});
 if (!result.err.empty()) std::cerr << result.err;
 if (result.exit_code != 0) throw support::ToolError("realpath failed", result.exit_code);
 return mmltk::tools::text::trim_newlines(std::move(result.out));
}
std::vector<fs::path> find_traces(const fs::path& build, bool& failed) {
 std::vector<fs::path> traces;
 std::vector<fs::path> pending{build};
 const auto report = [&](const fs::path& path, const std::error_code& error) {
  failed = true;
  std::cerr << "collect_time_traces: find " << path << ": " << error.message() << '\n';
 };
 while (!pending.empty()) {
  auto directory = std::move(pending.back());
  pending.pop_back();
  std::error_code error;
  fs::directory_iterator entry(directory, error), end;
  if (error) {
   report(directory, error);
   continue;
  }
  while (entry != end) {
   const auto path = entry->path();
   const auto status = entry->symlink_status(error);
   if (error)
    report(path, error);
   else if (fs::is_directory(status))
    pending.push_back(path);
   else if (fs::is_regular_file(status) && path.filename().native().ends_with(".json"))
    traces.push_back(path);
   entry.increment(error);
   if (error) {
    report(directory, error);
    break;
   }
  }
 }
 // C sort compares unsigned bytes, independently of the process locale.
 std::sort(traces.begin(), traces.end(), [](const fs::path& left, const fs::path& right) {
  const auto& a = left.native();
  const auto& b = right.native();
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) { return x < y; });
 });
 return traces;
}
void install_trace(const fs::path& source, fs::path destination) {
 std::error_code error;
 // Like install without -T, an existing destination directory receives
 // the source basename. Other existing destinations are replaced, so a
 // destination symlink or hard link is never written through.
 if (fs::is_directory(destination, error)) destination /= source.filename();
 if (fs::equivalent(source, destination, error)) throw support::ToolError(source.string() + " and " + destination.string() + " are the same file");
 std::vector<fs::path> missing;
 auto parent = destination.parent_path();
 while (!fs::exists(parent)) {
  missing.push_back(parent);
  parent = parent.parent_path();
 }
 for (auto directory = missing.rbegin(); directory != missing.rend(); ++directory) {
  if (fs::create_directory(*directory))
   fs::permissions(*directory, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec, fs::perm_options::replace);
 }
 const auto status = fs::symlink_status(destination, error);
 if (fs::is_directory(status)) throw support::ToolError("cannot overwrite directory " + destination.string());
 if (fs::exists(status)) fs::remove(destination);
 fs::copy_file(source, destination);
 fs::permissions(destination, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read, fs::perm_options::replace);
}
int collect(int argc, char** argv) {
 if (argc < 2 || argv[1][0] == '\0') throw support::ToolError("build directory is required");
 const auto build = canonical_missing(argv[1]);
 if (argc < 3 || argv[2][0] == '\0') throw support::ToolError("output directory is required");
 const auto output = canonical_missing(argv[2]);
 fs::create_directories(output);
 std::error_code error;
 if (!fs::is_directory(build, error)) return 0;
 bool failed = false;
 const auto traces = find_traces(build, failed);
 for (const auto& trace : traces) {
  auto stem = trace.native();
  stem.resize(stem.size() - std::string_view(".json").size());
  if (!support::regular_file(stem + ".o") && !support::regular_file(stem + ".obj")) continue;
  install_trace(trace, output / trace.lexically_relative(build));
 }
 return failed ? 1 : 0;
}
}  // namespace
export int mmltk_tool_collect_time_traces(int argc, char** argv) {
 try {
  return collect(argc, argv);
 } catch (const support::ToolError& error) {
  std::cerr << "collect_time_traces: " << error.what() << '\n';
  return error.status;
 } catch (const std::exception& error) {
  std::cerr << "collect_time_traces: " << error.what() << '\n';
  return 1;
 }
}
