module;
// Native counterpart of run_uncomment.sh. Requires cpp_support.hpp, the
// vendored nlohmann JSON headers, and OpenSSL libcrypto. Only comment removal
// itself is delegated to the existing uncomment executable.
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
export module mmltk.tools.run_uncomment;
import std;
import mmltk.tools.cpp_digest;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_linux;
import mmltk.tools.cpp_text;
namespace {
namespace support = mmltk::tools;
namespace fs = std::filesystem;
using support::json;
using support::ToolError;
bool executable_available(const std::string& command) {
 const auto executable = [](const fs::path& path) {
  std::error_code error;
  return fs::is_regular_file(path, error) && ::access(path.c_str(), X_OK) == 0;
 };
 if (command.find('/') != std::string::npos) return executable(command);
 for (const auto& directory : support::split(support::env("PATH"), ':')) {
  if (executable(fs::path(directory) / command)) return true;
 }
 return false;
}
using support::FileHasher;
std::vector<fs::path> manifests(const fs::path& root) {
 std::vector<fs::path> result;
 if (!fs::is_directory(root)) return result;
 for (const auto& entry : fs::directory_iterator(root)) {
  if (!entry.is_directory()) continue;
  const auto path = entry.path() / ".cargo-checksum.json";
  // glob includes an existing matching path even when it is not a file;
  // the later read then reports the failure.
  if (fs::symlink_status(path).type() != fs::file_type::not_found) result.push_back(path);
 }
 std::sort(result.begin(), result.end());
 return result;
}
std::vector<fs::path> crate_files(const fs::path& root) {
 std::vector<fs::path> result;
 // Like Path.rglob, do not descend into symlink directories; symlink files
 // still participate, with their target contents and lexical relative name.
 for (const auto& entry : fs::recursive_directory_iterator(root)) {
  const auto name = entry.path().filename().string();
  if (name == ".cargo-checksum.json" || name.starts_with(".cargo-checksum.json.")) continue;
  if (entry.is_regular_file()) result.push_back(entry.path());
 }
 // filesystem::path compares components, as pathlib does, rather than the
 // flattened string (which would sort a/b differently relative to a.c).
 std::sort(result.begin(), result.end());
 return result;
}
const json& file_entries(const json& data) {
 static const json empty = json::object();
 if (!data.is_object()) throw ToolError("Cargo checksum manifest must be an object");
 const auto found = data.find("files");
 if (found == data.end()) return empty;
 if (!found->is_object()) throw ToolError("Cargo checksum files must be an object");
 return *found;
}
void replace_manifest(const fs::path& path, std::string_view rendered) {
 const auto metadata = support::file_metadata(path);
 std::string pattern = (path.parent_path() / ".cargo-checksum.json.XXXXXX").string();
 support::FileDescriptor output(::mkstemp(pattern.data()));
 if (output.get() < 0) throw std::system_error(errno, std::generic_category(), "mkstemp " + path.string());
 struct TemporaryFile {
  fs::path path;
  ~TemporaryFile() {
   std::error_code error;
   fs::remove(path, error);
  }
 } temporary{pattern};
 support::write_all(output.get(), rendered, "write " + pattern);
 if (::fchmod(output.get(), metadata.st_mode) != 0) throw std::system_error(errno, std::generic_category(), "chmod " + pattern);
 if (::fchown(output.get(), metadata.st_uid, metadata.st_gid) != 0 && errno != EPERM && errno != EACCES) throw std::system_error(errno, std::generic_category(), "chown " + pattern);
 const int descriptor = output.release();
 if (::close(descriptor) != 0) throw std::system_error(errno, std::generic_category(), "close " + pattern);
 fs::rename(temporary.path, path);
}
int update_and_verify_crates(const fs::path& root) {
 FileHasher digest;
 std::size_t changed_manifests = 0;
 std::size_t changed_entries = 0;
 for (const auto& manifest : manifests(root)) {
  const auto original = support::read_text(manifest);
  auto data = json::parse(original);
  const auto& old_files = file_entries(data);
  auto files = json::object();
  for (const auto& path : crate_files(manifest.parent_path())) {
   const auto relative = path.lexically_relative(manifest.parent_path()).generic_string();
   const auto hash = digest(path);
   files[relative] = hash;
   const auto previous = old_files.find(relative);
   if (previous == old_files.end() || *previous != hash) ++changed_entries;
  }
  data["files"] = std::move(files);
  const auto rendered = data.dump(-1, ' ', false) + '\n';
  if (rendered == original) continue;
  replace_manifest(manifest, rendered);
  ++changed_manifests;
 }
 std::size_t manifest_count = 0;
 std::size_t entry_count = 0;
 std::vector<std::string> errors;
 const auto report = [&](std::string message) {
  // The script reports at most twenty errors, but checks every entry.
  if (errors.size() < 20) errors.push_back(std::move(message));
 };
 for (const auto& manifest : manifests(root)) {
  ++manifest_count;
  const auto data = support::read_json(manifest);
  const auto& files = file_entries(data);
  std::set<std::string> listed;
  for (auto iterator = files.begin(); iterator != files.end(); ++iterator) listed.insert(iterator.key());
  std::set<std::string> present;
  for (const auto& path : crate_files(manifest.parent_path())) present.insert(path.lexically_relative(manifest.parent_path()).generic_string());
  if (listed != present) report(manifest.string() + ": file set differs");
  for (auto iterator = files.begin(); iterator != files.end(); ++iterator) {
   ++entry_count;
   const auto path = manifest.parent_path() / iterator.key();
   if (!fs::is_regular_file(path)) {
    report(manifest.string() + ": missing " + iterator.key());
    continue;
   }
   if (iterator.value() != digest(path)) report(manifest.string() + ": hash mismatch for " + iterator.key());
  }
 }
 if (!errors.empty()) {
  for (const auto& error : errors) std::cerr << "run_uncomment: " << error << '\n';
  return 1;
 }
 std::cout << "run_uncomment: Firefox crate hashes updated_manifests=" << changed_manifests << " updated_entries=" << changed_entries << " verified_manifests=" << manifest_count
           << " verified_entries=" << entry_count << std::endl;
 return 0;
}
int run(const char* argv0) {
 const auto root = support::repository_root(argv0, __FILE__);
 const auto config = root / ".uncommentrc.toml";
 const auto firefox = root / "third_party/firefox";
 auto uncomment = support::env("UNCOMMENT_BIN", "uncomment");
 if (uncomment.empty()) uncomment = "uncomment";
 if (!executable_available(uncomment)) throw ToolError("uncomment is required");
 if (!fs::is_regular_file(config)) throw ToolError("missing " + config.string());
 if (!fs::is_directory(firefox)) throw ToolError("missing " + firefox.string());
 const auto cache = root / ".cache";
 fs::create_directories(cache);
 if (::chmod(cache.c_str(), 0755) != 0) throw std::system_error(errno, std::generic_category(), "chmod " + cache.string());
 // An absolute prefix keeps mkdtemp in the same repository-local cache as
 // the script, independently of TMPDIR.
 support::TemporaryDirectory work((cache / "uncomment.").string());
 const auto firefox_config = work.path() / "firefox.toml";
 fs::copy_file(config, firefox_config);
 {
  std::ofstream stream(firefox_config, std::ios::binary | std::ios::app);
  if (!stream) throw ToolError("cannot write " + firefox_config.string());
  stream << "\n[languages.rust]\nname = \"rust\"\nextensions = [\".rs\"]\n"
            "comment_nodes = [\"line_comment\", \"block_comment\"]\n"
            "doc_comment_nodes = [\"doc_comment\"]\nremove_docs = false\n";
  stream.close();
  if (!stream) throw ToolError("write failed: " + firefox_config.string());
 }
 std::cout << "run_uncomment: processing Firefox" << std::endl;
 const auto first = support::run({uncomment, "--config", firefox_config.string(), "--no-default-ignores", "--threads", "0", firefox.string()});
 if (first.exit_code) return first.exit_code;
 if (const int status = update_and_verify_crates(firefox / "third_party/rust")) return status;
 std::vector<std::string> command{uncomment, "--config", config.string(), "--no-default-ignores", "--threads", "0"};
 for (const auto& entry : fs::directory_iterator(root)) {
  const auto name = entry.path().filename().string();
  if (name != ".git" && name != ".cache" && name != "build" && name != "third_party") command.push_back(entry.path().string());
 }
 for (const auto& entry : fs::directory_iterator(root / "third_party")) {
  if (entry.path().filename() != "firefox") command.push_back(entry.path().string());
 }
 std::cout << "run_uncomment: processing the rest of the repository" << std::endl;
 const auto rest = support::run(command);
 if (rest.exit_code) return rest.exit_code;
 std::cout << "run_uncomment: complete" << std::endl;
 return 0;
}
}  // namespace
export int mmltk_tool_run_uncomment(int, char** argv) {
 // The original script does not consume or forward positional arguments.
 try {
  return run(argv[0]);
 } catch (const ToolError& error) {
  std::cerr << "run_uncomment: " << error.what() << '\n';
  return error.status;
 } catch (const std::exception& error) {
  std::cerr << "run_uncomment: " << error.what() << '\n';
  return 1;
 }
}
