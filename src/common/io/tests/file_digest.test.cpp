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
