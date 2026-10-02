#include "src/common/io/path_publication.h"
#include "src/common/system/tests/denied_syscall.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/syscall.h>
namespace {
namespace io = mmltk::common::io;
using mmltk::common::system::test_support::with_denied_syscall;
using mmltk::testsupport::write_text_file;
class PublicationFixture {
 const mmltk::testsupport::ScopedTempDir root{"path-publication"};
 const bool directory_;

public:
 explicit PublicationFixture(bool directory) : directory_(directory) { write_text_file(payload(staging), "candidate"); }
 const std::filesystem::path staging = root.path() / "staging";
 const std::filesystem::path output = root.path() / "output";
 std::filesystem::path payload(const std::filesystem::path& path) const { return directory_ ? path / "data" : path; }
 std::string contents(const std::filesystem::path& path) const {
  std::ifstream input(payload(path));
  return {std::istreambuf_iterator<char>{input}, {}};
 }
};
TEST_CASE("Atomic path publication admits new output and preserves occupied output without overwrite", "[common][io][publication]") {
 const PublicationFixture paths{GENERATE(false, true)};
 bool exists = false, overwrite = false;
 SECTION("new output without overwrite") {}
 SECTION("new output with overwrite") { overwrite = true; }
 SECTION("occupied output refuses no-replace") { exists = true; }
 SECTION("replacement exchanges and removes the old generation") { exists = overwrite = true; }
 if (exists) write_text_file(paths.payload(paths.output), "previous");
 if (exists && !overwrite) {
  CHECK_THROWS_WITH(io::publish_staged_path_atomically(paths.staging, paths.output, false), "publication destination already exists: " + paths.output.string());
  CHECK(paths.contents(paths.staging) == "candidate");
  CHECK(paths.contents(paths.output) == "previous");
 } else {
  const auto result = io::publish_staged_path_atomically(paths.staging, paths.output, overwrite);
  CHECK(result.replaced == exists);
  CHECK_FALSE(result.cleanup_error);
  CHECK(paths.contents(paths.output) == "candidate");
  CHECK_FALSE(std::filesystem::exists(paths.staging));
 }
}
TEST_CASE("Atomic publication treats empty directories and dangling symlinks as occupied", "[common][io][publication]") {
 const PublicationFixture paths{true};
 bool symlink = false;
 SECTION("empty directory") { std::filesystem::create_directory(paths.output); }
 SECTION("dangling symlink") {
  symlink = true;
  std::filesystem::create_symlink("absent", paths.output);
 }
 CHECK_THROWS(io::publish_staged_path_atomically(paths.staging, paths.output, false));
 CHECK(paths.contents(paths.staging) == "candidate");
 const auto status = std::filesystem::symlink_status(paths.output);
 CHECK(std::filesystem::is_symlink(status) == symlink);
 CHECK(std::filesystem::is_directory(status) == !symlink);
 if (symlink) CHECK(std::filesystem::read_symlink(paths.output) == "absent");
}
TEST_CASE("Denied publication parent sync restores candidate and previous generation", "[common][io][publication]") {
 const PublicationFixture paths{GENERATE(false, true)};
 const bool replaced = GENERATE(false, true);
 if (replaced) write_text_file(paths.payload(paths.output), "previous");
 CHECK(with_denied_syscall(SYS_fsync, [&] {
  try {
   (void)io::publish_staged_path_atomically(paths.staging, paths.output, replaced);
  } catch (const std::runtime_error& error) { return std::string_view{error.what()}.starts_with("fsync parent directory failed:"); }
  return false;
 }) == 0);
 CHECK(paths.contents(paths.staging) == "candidate");
 CHECK(std::filesystem::exists(paths.output) == replaced);
 if (replaced) CHECK(paths.contents(paths.output) == "previous");
}
TEST_CASE("Denied old-generation removal reports cleanup failure after committing publication", "[common][io][publication]") {
 const PublicationFixture paths{GENERATE(false, true)};
 write_text_file(paths.payload(paths.output), "previous");
 CHECK(with_denied_syscall(SYS_unlinkat, [&] {
  const auto result = io::publish_staged_path_atomically(paths.staging, paths.output);
  return result.replaced && result.cleanup_error == std::errc::operation_not_permitted;
 }) == 0);
 CHECK(paths.contents(paths.staging) == "previous");
 CHECK(paths.contents(paths.output) == "candidate");
}
}  // namespace
