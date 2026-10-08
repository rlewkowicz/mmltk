#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mmltk::tools::cleanup {

using Json = nlohmann::ordered_json;
using SourceReader = std::function<std::string(const std::string&)>;
using PathAvailable = std::function<bool(const std::string&)>;
using Clock = std::function<std::string()>;
using Range = std::array<std::int64_t, 2>;
using RangesByFile = std::map<std::string, std::vector<Range>>;
using Suppressions = std::map<std::string, Json>;

extern const std::filesystem::path REPO_ROOT;
extern const Json INVENTORY_COMMANDS;
extern const Json CLEANUP_PROFILES;
extern const Json AUTHORED_PROFILE;

std::size_t availableThreads();
std::string usageText();
Json rawCpdProfile(double minTokens = 12, std::string_view profileName = "cpp");
Json parseArgs(const std::vector<std::string>& args);
std::optional<std::string> authoredLanguage(std::string_view path);
bool profileIncludesPath(const Json& profile, std::string_view path);

struct DetectorOptions {
  std::optional<std::size_t> threads;
  std::optional<std::string> fileList;
};
Json detectorArguments(const Json& profile, const DetectorOptions& options = {});
Json collectInventoryInputs();
Json buildInventory(const Json& profile, const Json& inputs,
                    const PathAvailable& pathAvailable = {});

std::string decodeXmlAttribute(std::string_view value);
Json parseXmlAttributes(std::string_view tag);
struct ParseXmlOptions {
  std::optional<std::vector<std::string>> expectedPaths;
  std::function<std::string(const std::string&)> normalizePath;
};
Json parseCpdXml(std::string_view xml, const ParseXmlOptions& options = {});

struct SnapshotEntry {
  std::string source;
  std::string sha256;
};
using Snapshot = std::map<std::string, SnapshotEntry>;
Snapshot sourceSnapshot(const std::vector<std::string>& paths);
void requireUnchangedSources(const Snapshot& snapshot);
Json runCpd(const Json& profile, const std::vector<std::string>& paths);
Json runCpd(const Json& profile, const std::vector<std::string>& paths,
            const Snapshot& snapshot);

std::string repoRelativePath(std::string_view path,
                             std::string_view repoRoot = REPO_ROOT.native());
std::vector<Range> mergeOverlappingRanges(std::vector<Range> ranges);
std::string commonDirectory(const std::vector<std::string>& paths);
Json makeHit(std::int64_t lineCount, std::optional<std::int64_t> tokenCount,
             const RangesByFile& rangesByFile, bool mergeRanges);
Json structuralHits(const Json& profile, const Json& duplications,
                    const std::vector<std::string>& scannedPaths,
                    std::string_view repoRoot = REPO_ROOT.native());
std::int64_t duploLineCount(const Json& hit);
Json duploOccurrence(const Json& hit, int side);
std::string stripCommentsAndStrings(std::string_view source);
Json filterDuploCandidates(const Json& profile, const Json& rawHits,
                           const SourceReader& sourceReader = {});
Json compactDuploHits(const Json& profile, const Json& rawHits,
                      const std::vector<std::string>& scannedPaths);
int compareHits(const Json& left, const Json& right);
Json summarize(const Json& hits);
Suppressions parseInlineSuppressions(std::string_view path, std::string_view source);
Json applyInlineSuppressions(const Json& hits, const Suppressions& suppressions,
                              std::string_view detector);
void requireAtomicSuppressionUse(const Json& suppressedOccurrences);
Json parseDuploJson(std::string_view text);

// Input contains the original report fields, with an optional integer threads.
// The injected clock supplies an ISO-8601 timestamp (UTC, millisecond precision).
Json buildReport(const Json& input, const Clock& clock = {});
std::string serializeReport(const Json& report);
Json rejectionReport(const Json& previous, const Json& report,
                      const Json& filteredCpd);
void writeAtomic(const std::filesystem::path& path, std::string_view contents);

struct GenerateReportOptions {
  Clock clock;
  std::optional<Json> inventoryInputs;
  PathAvailable pathAvailable;
};
Json generateReport(const Json& profile, const GenerateReportOptions& options = {});
Json rawCpdReport(const Json& profile, const Json& files, const Json& cpd,
                  const SourceReader& sourceReader = {});
struct GenerateRawCpdOptions {
  bool review = false;
  std::optional<Json> inventoryInputs;
  // A JSON return value retains validation of non-string renderer results.
  std::function<Json(const Json&)> renderMarkdown;
};
Json generateRawCpd(const Json& profile, std::string_view output,
                    const GenerateRawCpdOptions& options = {});
int main(const std::vector<std::string>& args);

}  // namespace mmltk::tools::cleanup
