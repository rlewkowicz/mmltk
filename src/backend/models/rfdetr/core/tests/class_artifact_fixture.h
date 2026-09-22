#pragma once
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/common/io/file_digest.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
namespace mmltk::backend::models::rfdetr::test_support {
class ClassArtifactFixture final {
public:
 ClassArtifactFixture(std::filesystem::path artifact, std::string_view bytes, const ModelClassLayout& layout) : artifact_(std::move(artifact)), companion_(artifact_.string() + ".classes.json") {
  {
   std::ofstream output(artifact_);
   output << bytes;
  }
  artifact_digest_ = mmltk::common::io::sha256_file(artifact_);
  {
   std::ofstream output(companion_);
   output << encode_class_descriptor({1, mmltk::common::io::sha256_hex(artifact_digest_), layout});
  }
  companion_digest_ = mmltk::common::io::sha256_file(companion_);
 }
 void CheckPreserved() const {
  CHECK(mmltk::common::io::sha256_file(artifact_) == artifact_digest_);
  CHECK(mmltk::common::io::sha256_file(companion_) == companion_digest_);
  for (const auto& entry : std::filesystem::directory_iterator(artifact_.parent_path())) CHECK_FALSE(entry.is_directory());
 }

private:
 std::filesystem::path artifact_;
 std::filesystem::path companion_;
 mmltk::common::io::Sha256Digest artifact_digest_;
 mmltk::common::io::Sha256Digest companion_digest_;
};
}  // namespace mmltk::backend::models::rfdetr::test_support
