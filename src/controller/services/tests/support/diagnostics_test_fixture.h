#pragma once
#include "src/controller/services/diagnostics/diagnostics_client.h"
#include "src/common/io/scoped_fd.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fcntl.h>
namespace mmltk::controller::test_support {
struct DiagnosticFileFixture final {
 const std::filesystem::path log;
 services::DiagnosticsClient client;
 explicit DiagnosticFileFixture(const std::filesystem::path& root)
     : log(root / "trace.jsonl"), client(mmltk::common::io::ScopedFd{::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600)}, services::DiagnosticsExecutionPolicy::CallerDriven) {
  REQUIRE(client.enabled());
 }
};
}  // namespace mmltk::controller::test_support
