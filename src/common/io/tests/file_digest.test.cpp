#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "src/common/io/file_digest.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <fstream>
namespace io = mmltk::common::io;
namespace {
class DigestFileFixture {
private:
 const mmltk::testsupport::ScopedTempDir temporary{"digest"};

protected:
 DigestFileFixture() { mmltk::testsupport::write_text_file(path, "abc"); }
 void Replace() const {
  const auto replacement = temporary.path() / "replacement";
  mmltk::testsupport::write_text_file(replacement, "abc");
  std::filesystem::rename(replacement, path);
 }
 const std::filesystem::path path = temporary.path() / "input";
};
}  // namespace
TEST_CASE_METHOD(DigestFileFixture, "File digest shares MD5 and SHA256 traversal with stable admission", "[digest]") {
 const auto digests = io::try_file_digests(path, true);
 REQUIRE(digests);
 CHECK(digests->md5 == "900150983cd24fb0d6963f7d28e17f72");
 CHECK(io::sha256_hex(digests->sha256) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
 CHECK(io::parse_sha256_hex(io::sha256_hex(digests->sha256)) == digests->sha256);
 CHECK_NOTHROW(digests->snapshot.RequireUnchanged(path));
 Replace();
 CHECK_THROWS(digests->snapshot.RequireUnchanged(path));
 CHECK_FALSE(io::try_sha256_file(path, [] { return true; }));
 CHECK_THROWS(io::parse_sha256_hex("not a digest"));
}
TEST_CASE("Incremental SHA256 preserves chunk boundaries and sealed finalization", "[digest]") {
 io::Sha256Hasher hash;
 const std::array<std::uint8_t, 3> bytes{'a', 'b', 'c'};
 hash.Update({});
 hash.Update(std::span(bytes).first(1));
 hash.Update(std::span(bytes).subspan(1));
 CHECK(io::sha256_hex(hash.Finish()) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
 CHECK_THROWS_AS(hash.Finish(), std::logic_error);
 CHECK_THROWS_AS(hash.Update(bytes), std::logic_error);
 io::Sha256Hasher empty;
 CHECK(io::sha256_hex(empty.Finish()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
 CHECK(io::sha256_bytes(bytes) == io::parse_sha256_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}
TEST_CASE_METHOD(DigestFileFixture, "File digest handles empty files and complete multi-chunk input", "[digest]") {
 { std::ofstream file(path, std::ios::binary); }
 const auto empty = io::try_file_digests(path, true);
 REQUIRE(empty);
 CHECK(empty->md5 == "d41d8cd98f00b204e9800998ecf8427e");
 CHECK(io::sha256_hex(empty->sha256) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
 std::array<std::uint8_t, 4096U> block{};
 for (std::size_t index = 0U; index < block.size(); ++index) block[index] = static_cast<std::uint8_t>(index);
 io::Sha256Hasher expected;
 {
  std::ofstream file(path, std::ios::binary);
  for (std::size_t index = 0U; index < 4096U; ++index) {
   file.write(reinterpret_cast<const char*>(block.data()), block.size());
   expected.Update(block);
  }
  file.write(reinterpret_cast<const char*>(block.data()), 3);
  expected.Update(std::span{block}.first(3U));
  REQUIRE(file.good());
 }
 const auto digest = io::try_file_digests(path, true);
 REQUIRE(digest);
 CHECK(digest->snapshot.bytes == 16U * 1024U * 1024U + 3U);
 CHECK(digest->sha256 == expected.Finish());
 const auto sha_only = io::try_file_digests(path, false);
 REQUIRE(sha_only);
 CHECK(sha_only->md5.empty());
 CHECK(sha_only->sha256 == digest->sha256);
 for (int stop_checkpoint = 1; stop_checkpoint <= 4; ++stop_checkpoint) {
  int checkpoints = 0;
  CHECK_FALSE(io::try_file_digests(path, true, [&] { return ++checkpoints == stop_checkpoint; }));
  CHECK(checkpoints == stop_checkpoint);
 }
 CHECK_THROWS_WITH(io::sha256_file(path, [] { return true; }), "file digest cancelled");
}
TEST_CASE_METHOD(DigestFileFixture, "File digest rejects mutation and replacement during admission", "[digest]") {
 int checkpoints = 0;
 SECTION("truncation before the read cannot expose unwritten scratch") {
  CHECK_THROWS_WITH(io::try_file_digests(path, true,
                     [&] {
                      if (++checkpoints == 2) std::filesystem::resize_file(path, 1U);
                      return false;
                     }),
   "unexpected EOF during pread");
 }
 SECTION("growth after the read fails descriptor admission") {
  CHECK_THROWS_WITH(io::try_file_digests(path, true,
                     [&] {
                      if (++checkpoints == 3) {
                       std::ofstream file(path, std::ios::app);
                       file << "d";
                      }
                      return false;
                     }),
   "artifact changed while computing digest");
 }
 SECTION("replacement before final admission fails pathname custody") {
  CHECK_THROWS(io::try_file_digests(path, true, [&] {
   if (++checkpoints == 3) Replace();
   return false;
  }));
 }
}
