#include <catch2/catch_test_macros.hpp>
#include "src/common/io/file_digest.h"
#include "src/common/io/staging_directory.h"
#include <fstream>
namespace io = mmltk::common::io;
TEST_CASE("File digest shares MD5 and SHA256 traversal with stable admission", "[digest]") {
    io::StagingDirectory temporary(std::filesystem::temp_directory_path() / "digest", ".", "-XXXXXX", "digest fixture");
    const auto path = temporary.path() / "input";
    {
        std::ofstream file(path);
        file << "abc";
    }
    const auto digests = io::try_file_digests(path, true);
    REQUIRE(digests);
    CHECK(digests->md5 == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(io::sha256_hex(digests->sha256) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(io::parse_sha256_hex(io::sha256_hex(digests->sha256)) == digests->sha256);
    CHECK_NOTHROW(digests->snapshot.RequireUnchanged(path));
    {
        std::ofstream file(temporary.path() / "replacement");
        file << "abc";
    }
    std::filesystem::rename(temporary.path() / "replacement", path);
    CHECK_THROWS(digests->snapshot.RequireUnchanged(path));
    CHECK_FALSE(io::try_sha256_file(path, [] { return true; }));
    CHECK_THROWS(io::parse_sha256_hex("not a digest"));
}
TEST_CASE("Incremental SHA256 preserves chunk boundaries and sealed finalization", "[digest]") {
    io::Sha256Hasher hash;
    const std::array<std::uint8_t,3> bytes{'a','b','c'};
    hash.Update({}); hash.Update(std::span(bytes).first(1)); hash.Update(std::span(bytes).subspan(1));
    CHECK(io::sha256_hex(hash.Finish())=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_THROWS_AS(hash.Finish(),std::logic_error); CHECK_THROWS_AS(hash.Update(bytes),std::logic_error);
    io::Sha256Hasher empty;
    CHECK(io::sha256_hex(empty.Finish())=="e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(io::sha256_bytes(bytes)==io::parse_sha256_hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}
