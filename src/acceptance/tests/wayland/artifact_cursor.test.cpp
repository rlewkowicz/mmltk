#include "artifact_cursor.h"
#include "audit_facts.h"
#include "browser_audit.h"
#include "src/test_support/filesystem_test_utils.hpp"
#include <nlohmann/json.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "session.h"
namespace mmltk::acceptance::wayland {
using mmltk::testsupport::ScopedTempDir;
TEST_CASE("acceptance artifacts have independent writers and one process-family archive identity", "[workspace][audit]") {
 ScopedTempDir temporary{"mmltk-evidence-ownership"};
 const auto native = temporary.path() / "capture.jsonl";
 const auto parent = artifact_sibling(native, "-acceptance.jsonl");
 const auto application = artifact_sibling(native, "-application.log");
 const auto mozilla = artifact_sibling(native, "-mozilla-child.12.log.child-4.moz_log.0");
 const auto mozilla_parent = artifact_sibling(native, "-mozilla-main.11.log.moz_log.3");
 const auto adjacent = temporary.path() / "capture-other-mozilla-child.13.log.child-5.moz_log.0";
 {
  std::ofstream output{adjacent};
  output << "adjacent capture\n";
 }
 for (const auto& path : {native, parent, application, mozilla, mozilla_parent}) {
  std::ofstream output{path};
  output << "{\"event\":\"previous\"}\n";
 }
 const std::array independent{native, parent, application, mozilla, mozilla_parent};
 require_independent_artifacts(independent);
 const auto alias = temporary.path() / "alias.jsonl";
 std::filesystem::create_hard_link(native, alias);
 const std::array competing{native, alias};
 CHECK_THROWS_AS(require_independent_artifacts(competing), std::runtime_error);
 std::filesystem::remove(alias);
 prepare_latest_log(native, "native", "17-123");
 prepare_latest_log(parent, "acceptance", "17-123");
 rotate_process_log_family(native, "17-123");
 for (const auto& path : {native, parent, application, mozilla, mozilla_parent}) {
  const auto archive = std::filesystem::path{path.string() + ".history"} / (path.extension() == ".jsonl" ? "17-123.jsonl" : "17-123.log");
  CHECK(read_tail(archive) == "{\"event\":\"previous\"}\n");
 }
 CHECK_FALSE(std::filesystem::exists(application));
 CHECK_FALSE(std::filesystem::exists(mozilla));
 CHECK_FALSE(std::filesystem::exists(mozilla_parent));
 CHECK(read_tail(adjacent) == "adjacent capture\n");
 append_acceptance_record(parent, {{"event", "acceptance.complete"}});
 CHECK(std::filesystem::file_size(native) == 0U);
 CHECK(read_tail(parent).contains("acceptance.complete"));
}
TEST_CASE("native evidence cursors reject malformed truncated oversized and incomplete records", "[workspace][audit]") {
 struct Count final {
  std::size_t records = 0U;
  void consume(const nlohmann::json&) { ++records; }
 };
 ScopedTempDir temporary{"mmltk-evidence-cursor"};
 const auto path = temporary.path() / "native.jsonl";
 const auto observer = [](const auto&) {};
 for (const std::string& content : {std::string{"not JSON\n"}, std::string{"[]\n"}, std::string{"\n"}, std::string{"{\"event\":\"bad\0value\"}\n", 22U}, std::string(64U * 1024U + 1U, 'x')}) {
  {
   std::ofstream output{path, std::ios::binary | std::ios::trunc};
   output << content;
  }
  Count audit;
  JsonLineCursor cursor{path, 0U};
  CHECK_THROWS_AS(cursor.consume(audit, observer), std::runtime_error);
  CHECK(audit.records == 0U);
 }
 {
  std::ofstream output{path, std::ios::binary | std::ios::trunc};
  output << "{\"event\":\"partial";
 }
 Count audit;
 JsonLineCursor cursor{path, 0U};
 cursor.consume(audit, observer);
 cursor.consume(audit, observer, true);
 CHECK(audit.records == 0U);
 CHECK_THROWS_AS(cursor.finish(), std::runtime_error);
 {
  std::ofstream output{path, std::ios::binary | std::ios::app};
  output << "\"}\n";
 }
 cursor.consume(audit, observer);
 cursor.finish();
 CHECK(audit.records == 1U);
 { std::ofstream output{path, std::ios::binary | std::ios::trunc}; }
 CHECK_THROWS_AS(cursor.consume(audit, observer), std::runtime_error);
 std::filesystem::remove(path);
 CHECK_THROWS_AS(cursor.consume(audit, observer), std::runtime_error);
}
TEST_CASE("evidence reads cross chunk boundaries and failure tails select actual final bytes", "[workspace][audit]") {
 struct Count final {
  std::size_t records = 0U;
  void consume(const nlohmann::json&) { ++records; }
 };
 ScopedTempDir temporary{"mmltk-evidence-chunks"};
 const auto path = temporary.path() / "native.jsonl";
 constexpr std::size_t count = 12000U;
 {
  std::ofstream output{path};
  for (std::size_t index = 0U; index != count; ++index) output << "{\"event\":\"chunk\"}\n";
 }
 Count audit;
 JsonLineCursor cursor{path, 0U};
 cursor.consume(audit, [](const auto&) {});
 cursor.finish();
 CHECK(audit.records == count);
 {
  std::ofstream output{path, std::ios::app};
  output << "FINAL TAIL\n";
 }
 const auto tail = read_tail(path);
 CHECK(tail.size() == 96U * 1024U);
 CHECK(tail.ends_with("FINAL TAIL\n"));
 const auto firefox = temporary.path() / "firefox.log";
 {
  std::ofstream output{firefox};
  output << "\nintentional Firefox text\nprefix {\"event\":\"browser\"}\n";
 }
 Count browser;
 JsonLineCursor browser_cursor{firefox, 0U, JsonLineCursor::Format::FirefoxText};
 browser_cursor.consume(browser, [](const auto&) {});
 CHECK(browser.records == 1U);
 const std::array failures{
  "Uncaptured WebGPU error: Texture TextureId(1,1) is invalid",
  "XPCOMGlueLoad error: dependency unavailable",
  "Couldn't load XPCOM",
  "thread 'main' panicked at failure",
  "{\"event\":\"integration.failed\"}",
  "{\"event\":\"browser.panic\"}",
  "{\"event\":\"browser.invalid_webgpu_texture\"}",
  "{\"event\":\"firefox.workspace.channel_terminal\",\"terminal\":\"protocol_failure\"}",
 };
 for (const bool viewer : {false, true}) {
  const nlohmann::json completion{
   {"event", viewer ? "integration.viewer_complete" : "integration.complete"},
   {"detail", viewer ? "square" : "typed-mvc-wayland"},
   {"a", 1U},
   {"b", 1U},
   {"c", 1U},
   {"d", 1U},
  };
  for (const auto* failure : failures) {
   for (const std::string_view order : {"failure-first", "completion-first", "final-drain", "older-than-tail"}) {
    INFO("viewer: " << viewer << ", failure: " << failure << ", order: " << order);
    {
     std::ofstream output{firefox, std::ios::trunc};
     if (order == "failure-first" || order == "older-than-tail") output << failure << '\n';
     if (order == "older-than-tail")
      for (std::size_t index = 0U; index != count; ++index) output << "Firefox ordinary text\n";
     output << completion.dump() << '\n';
     if (order == "completion-first") output << failure << '\n';
    }
    BrowserAudit evidence;
    JsonLineCursor evidence_cursor{firefox, 0U, JsonLineCursor::Format::FirefoxText};
    evidence_cursor.consume(evidence, [](const auto&) {});
    CHECK((viewer ? evidence.viewer_complete : evidence.complete));
    if (order == "final-drain") {
     CHECK_FALSE(evidence.failed_before_termination());
     {
      std::ofstream output{firefox, std::ios::app};
      output << failure;
     }
     // Final Firefox text need not have a newline. Ordinary reads
     // retain it until terminal settlement supplies the last extent.
     evidence_cursor.consume(evidence, [](const auto&) {});
     CHECK_FALSE(evidence.failed_before_termination());
     evidence_cursor.consume(evidence, [](const auto&) {}, true);
    }
    if (order == "older-than-tail") CHECK_FALSE(read_tail(firefox).contains(failure));
    // Both readiness exits and terminal settlement use this owner,
    // independently of their ordinary/viewer completion evidence.
    CHECK(evidence.failed_before_termination());
    CHECK_FALSE(evidence.failure_blocker().empty());
    evidence_cursor.consume(evidence, [](const auto&) {}, true);
    CHECK(evidence.failed_before_termination());
   }
  }
 }
}
TEST_CASE("evidence cursor admits exact line limits and retains admitted overflow prefixes", "[workspace][audit]") {
 struct Records final {
  std::vector<nlohmann::json> values;
  void consume(const nlohmann::json& value) { values.push_back(value); }
 };
 ScopedTempDir temporary{"mmltk-evidence-limits"};
 const auto path = temporary.path() / "native.jsonl";
 constexpr std::size_t limit = 64U * 1024U;
 const auto observer = [](const auto&) {};
 for (const std::size_t split : {0U, 16383U, 16384U, 65536U}) {
  for (const bool overflow : {false, true}) {
   INFO("split=" << split << ", overflow=" << overflow);
   std::string record = "{\"event\":\"limit\"}";
   record.resize(limit, ' ');
   mmltk::testsupport::write_text_file(path, std::string_view{record}.substr(0U, split));
   Records audit;
   JsonLineCursor cursor{path, 0U};
   cursor.consume(audit, observer);
   {
    std::ofstream output{path, std::ios::binary | std::ios::app};
    output << std::string_view{record}.substr(split);
    if (overflow) output << 'x';
   }
   if (overflow) {
    CHECK_THROWS_WITH(cursor.consume(audit, observer), "evidence line exceeded capacity: " + path.string());
   } else {
    cursor.consume(audit, observer);
   }
   CHECK(audit.values.empty());
   CHECK(cursor.line() == 0U);
   CHECK_THROWS_AS(cursor.finish(), std::runtime_error);
   {
    std::ofstream output{path, std::ios::app};
    output << '\n';
   }
   cursor.consume(audit, observer);
   cursor.finish();
   REQUIRE(audit.values.size() == 1U);
   CHECK(audit.values.front().at("event") == "limit");
   CHECK(cursor.line() == 1U);
  }
 }
}
TEST_CASE("evidence cursor preserves callback failure order and captured extent", "[workspace][audit]") {
 struct Audit final {
  std::vector<std::string>* order;
  bool fail;
  void consume(const nlohmann::json& record) {
   order->push_back("audit:" + record.at("event").get<std::string>());
   if (fail) throw std::runtime_error("audit failure");
  }
 };
 ScopedTempDir temporary{"mmltk-evidence-callback"};
 const auto path = temporary.path() / "native.jsonl";
 for (const bool audit_failure : {false, true}) {
  mmltk::testsupport::write_text_file(path, "{\"event\":\"first\"}\n{\"event\":\"skipped\"}\n");
  std::vector<std::string> order;
  Audit audit{&order, audit_failure};
  JsonLineCursor cursor{path, 0U};
  CHECK_THROWS_AS(cursor.consume(audit,
                   [&](const nlohmann::json&) {
                    order.push_back("observer:first");
                    throw std::runtime_error("observer failure");
                   }),
   std::runtime_error);
  CHECK(order.size() == (audit_failure ? 1U : 2U));
  CHECK(order.front() == "audit:first");
  CHECK(cursor.line() == 1U);
  CHECK_THROWS_AS(cursor.finish(), std::runtime_error);
  audit.fail = false;
  {
   std::ofstream output{path, std::ios::app};
   output << '\n';
  }
  cursor.consume(audit, [&](const nlohmann::json& value) { order.push_back("observer:" + value.at("event").get<std::string>()); });
  CHECK(order[order.size() - 2U] == "audit:first");
  CHECK(order.back() == "observer:first");
  CHECK(cursor.line() == 2U);
  cursor.finish();
 }
 mmltk::testsupport::write_text_file(path, "{\"event\":\"first\"}\r\n");
 std::vector<std::string> order;
 Audit audit{&order, false};
 JsonLineCursor cursor{path, 0U};
 cursor.consume(audit, [&](const auto&) {
  std::ofstream output{path, std::ios::app};
  output << "{\"event\":\"later\"}\n";
 });
 CHECK(order == std::vector<std::string>{"audit:first"});
 cursor.consume(audit, [](const auto&) {});
 CHECK(order == std::vector<std::string>{"audit:first", "audit:later"});
 cursor.finish();
}
}  // namespace mmltk::acceptance::wayland
