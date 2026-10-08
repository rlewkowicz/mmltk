module;

#include <algorithm>
#include <cerrno>
#include <exception>
#include <iostream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include <sys/stat.h>

#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
#include <cstddef>
#include <functional>
#include <optional>
#include <string_view>
#include <initializer_list>
#include <array>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>

export module mmltk.tools.format_declarations;
import mmltk.tools.cleanup.cpd_patterns;
import mmltk.tools.cleanup.declaration_patterns;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
import mmltk.tools.generate_cleanup_json;

export {
namespace mmltk::tools::formatter {

using Json = nlohmann::ordered_json;

Json parseFormatArgs(const std::vector<std::string>& args);
Json declarationFormatReport(const std::vector<std::string>& inventory,
                             const std::map<std::string, std::string>& sources,
                             const Json& options = Json::object());
int runFormatter(const Json& options);
int main(const std::vector<std::string>& args);

}  // namespace mmltk::tools::formatter

}

namespace mmltk::tools::formatter {

Json parseFormatArgs(const std::vector<std::string>& args) {
  if (args.size() == 1 && (args.front() == "--help" || args.front() == "-h"))
    return {{"mode", "help"}};
  if (args.empty() || (args.front() != "check" && args.front() != "preview" && args.front() != "fix"))
    throw std::runtime_error("expected check, preview, or fix; use --help");
  Json options = {{"mode", args.front()}, {"files", Json::array()},
                  {"report", "cleanup/declarations-format.json"}};
  bool reportSeen = false;
  for (std::size_t i = 1; i < args.size(); i += 2) {
    if ((args[i] != "--file" && args[i] != "--report") || i + 1 >= args.size() || args[i + 1].empty())
      throw std::runtime_error("expected --file PATH or --report PATH");
    if (args[i] == "--file") options["files"].push_back(args[i + 1]);
    else {
      if (reportSeen) throw std::runtime_error("--report may appear only once");
      options["report"] = args[i + 1];
      reportSeen = true;
    }
  }
  return options;
}

Json declarationFormatReport(const std::vector<std::string>& inventory,
                             const std::map<std::string, std::string>& sources,
                             const Json& options) {
  const auto conflicts = declarations::inventoryMacroConflicts(sources);
  auto selected = inventory;
  const auto requested = options.find("files");
  if (requested != options.end() && !requested->is_null() && !requested->empty()) {
    selected = requested->get<std::vector<std::string>>();
    // Array.sort() orders UTF-16 code units, including non-ASCII filenames.
    std::sort(selected.begin(), selected.end(), [](const auto& left, const auto& right) {
      return cpd::toUtf16(left) < cpd::toUtf16(right);
    });
    selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  }
  const std::set<std::string> available(inventory.begin(), inventory.end());
  for (const auto& path : selected)
    if (!available.contains(path))
      throw std::runtime_error("source is not in the first-party CPD inventory: " + path);

  Json changedFiles = Json::array(), files = Json::array();
  Json summary = {{"files", selected.size()}, {"changed_files", 0},
                  {"rewrite", 0}, {"manual", 0}, {"retained", 0}};
  const declarations::Options formatOptions{conflicts};
  for (const auto& path : selected) {
    const auto& source = sources.at(path);
    const auto result = declarations::formatDeclarationSource(path, source, formatOptions);
    const bool changed = result.at("changed").get<bool>();
    if (changed) changedFiles.push_back(path);
    const auto& candidates = result.at("candidates");
    for (const auto& candidate : candidates) {
      const auto disposition = candidate.value("disposition", std::string{});
      if (disposition == "rewrite" || disposition == "manual" || disposition == "retained")
        summary[disposition] = summary.at(disposition).get<std::size_t>() + 1;
    }
    if (!candidates.empty()) {
      Json file = {{"path", path}, {"edits", result.at("edits")}, {"candidates", candidates}};
      if (changed) {
        file["before"] = source;
        file["after"] = result.at("output");
      }
      files.push_back(std::move(file));
    }
  }
  summary["changed_files"] = changedFiles.size();
  // Conflict names are ASCII macro identifiers, so set order equals JS sort.
  return {{"format", 1},
          {"purpose", "Syntactic annotation formatting; manual candidates are not duplication verdicts or automatic permission"},
          {"inventory", selected}, {"macro_conflicts", conflicts},
          {"changed_files", std::move(changedFiles)}, {"files", std::move(files)},
          {"summary", std::move(summary)}};
}

int runFormatter(const Json& options) {
  const auto reportPath = options.at("report").get<std::string>();
  if (!reportPath.ends_with(".json"))
    throw std::runtime_error("formatter report must be a .json path");
  const auto inventory = cleanup::buildInventory(cleanup::CLEANUP_PROFILES.at("cpp"),
                                                 cleanup::collectInventoryInputs())
                             .at("scannedFiles").get<std::vector<std::string>>();
  std::map<std::string, std::string> sources;
  for (const auto& path : inventory) sources.emplace(path, read_text(path));
  const auto report = declarationFormatReport(inventory, sources, options);
  // Publish original evidence before any rewrite, independently of raw CPD.
  cleanup::writeAtomic(reportPath, json_dump(report, 2) + '\n');
  const auto mode = options.at("mode").get<std::string>();
  if (mode == "fix") {
    for (const auto& file : report.at("files")) {
      if (!file.contains("after")) continue;
      const auto path = file.at("path").get<std::string>();
      if (read_text(path) != sources.at(path))
        throw std::runtime_error("source changed while formatting: " + path);
      struct stat status {};
      if (::stat(path.c_str(), &status) != 0)
        throw std::system_error(errno, std::generic_category(), "stat " + path);
      cleanup::writeAtomic(path, file.at("after").get_ref<const std::string&>());
      if (::chmod(path.c_str(), status.st_mode) != 0)
        throw std::system_error(errno, std::generic_category(), "chmod " + path);
    }
  }
  const auto& summary = report.at("summary");
  std::cout << mode << ": " << summary.at("changed_files") << " files, "
            << summary.at("rewrite") << " safe rewrites, " << summary.at("manual")
            << " manual candidates; " << reportPath << '\n';
  if (mode == "preview")
    for (const auto& path : report.at("changed_files"))
      std::cout << "  " << path.get_ref<const std::string&>() << '\n';
  return mode == "check" && !report.at("changed_files").empty() ? 1 : 0;
}

int main(const std::vector<std::string>& args) {
  try {
    const auto options = parseFormatArgs(args);
    if (options.at("mode") == "help") {
      std::cout << "Usage: ./mmltk --format-declarations check|preview|fix [--file PATH ...] [--report PATH]\n"
                   "Default: complete first-party CPD inventory. Check exits 1 for safe pending edits; preview leaves sources unchanged.\n"
                   "Every mode writes original spans, reasons, proposed edits and explicit manual candidates.\n"
                   "Fix changes only proven canonical policy constructions and adds their direct include. Repeated fix is idempotent.\n"
                   "Unknown lookup, conflicting macros, preprocessing, comments inside annotations and unfamiliar compositions remain manual.\n";
      return 0;
    }
    return runFormatter(options);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}

}  // namespace mmltk::tools::formatter

export int mmltk_tool_format_declarations(int argc, char** argv) {
  return mmltk::tools::formatter::main(std::vector<std::string>(argv + 1, argv + argc));
}
