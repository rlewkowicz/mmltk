// Lexical evidence, not an include-what-you-use or semantic dependency checker.
#include "audit_includes.hpp"

#include "cleanup/cpd_patterns.hpp"
#include "cleanup/declaration_patterns.hpp"
#include "cpp_locale.hpp"
#include "cpp_support.hpp"
#include "generate_cleanup_json.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mmltk::tools::include_audit {
namespace {

const std::string& string(const Json& value) { return value.get_ref<const std::string&>(); }

using mmltk::tools::text::whitespace;

using cpd::jsTrim;

std::string resolvePath(std::string_view directory, std::string_view path) {
  return std::filesystem::absolute(std::filesystem::path(directory) / std::filesystem::path(path))
      .lexically_normal().generic_string();
}

std::string relativePath(std::string_view root, std::string_view path) {
  const auto from = std::filesystem::absolute(std::filesystem::path(root)).lexically_normal();
  const auto to = std::filesystem::absolute(std::filesystem::path(path)).lexically_normal();
  const auto relative = to.lexically_relative(from).generic_string();
  return relative == "." ? "" : relative;
}

std::string dirname(std::string_view path) {
  const auto parent = std::filesystem::path(path).parent_path().generic_string();
  return parent.empty() ? "." : parent;
}

bool matches(std::string_view text, const std::regex& pattern) {
  return std::regex_search(text.begin(), text.end(), pattern);
}

bool isPch(std::string_view path) {
  static const std::regex pattern(R"(^src/pch_\w+\.h$)");
  return matches(path, pattern);
}

bool ordinaryCpp(std::string_view path) {
  static const std::regex pattern(R"(\.(?:cpp|cc|cxx|c\+\+|C)$)");
  return matches(path, pattern);
}

bool has(const Json& array, std::string_view value) {
  return std::any_of(array.begin(), array.end(), [&](const Json& item) { return item == value; });
}

bool lexicalLess(const Json& a, const Json& b) {
  return cpd::toUtf16(string(a)) < cpd::toUtf16(string(b));
}

Json sorted(Json array) {
  std::sort(array.begin(), array.end(), lexicalLess);
  array.erase(std::unique(array.begin(), array.end()), array.end());
  return array;
}

Json sortedFileRecords(const Json& records) {
  auto result = Json::array();
  for (auto record : records) {
    record["files"] = sorted(record.at("files"));
    result.push_back(std::move(record));
  }
  return result;
}


std::string join(const Json& array, std::string_view separator) {
  std::string result;
  bool first = true;
  for (const auto& item : array) {
    if (!first) result += separator;
    result += string(item);
    first = false;
  }
  return result;
}

void addUnique(Json& array, const Json& value) {
  if (std::find(array.begin(), array.end(), value) == array.end()) array.push_back(value);
}

const Json& contextsFor(const Json& contexts, std::string_view path) {
  static const Json empty = Json::array();
  const auto found = contexts.find(std::string(path));
  return found == contexts.end() ? empty : *found;
}

std::set<std::string> words(std::string_view text) {
  std::istringstream input{std::string(text)};
  return {std::istream_iterator<std::string>(input), std::istream_iterator<std::string>()};
}

std::string systemHeaderCategory(const Json& include) {
  if (include.at("spelling") != "angle") return {};
  static const auto standard = words(R"(
algorithm any array atomic barrier bit bitset cassert cctype cerrno cfenv cfloat charconv chrono cinttypes
climits clocale cmath codecvt compare complex concepts condition_variable contracts coroutine csetjmp csignal
cstdarg cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype deque exception execution expected
filesystem flat_map flat_set format forward_list fstream functional future generator hazard_pointer
initializer_list inplace_vector iomanip ios iosfwd iostream istream iterator latch limits list locale map
mdspan memory memory_resource meta mutex new numbers numeric optional ostream print queue random ranges ratio
rcu regex scoped_allocator semaphore set shared_mutex source_location span spanstream sstream stack stacktrace
stdexcept stdfloat stop_token streambuf string string_view strstream syncstream system_error text_encoding
thread tuple type_traits typeindex typeinfo unordered_map unordered_set utility valarray variant vector
version assert.h complex.h ctype.h errno.h fenv.h float.h inttypes.h limits.h locale.h math.h setjmp.h
signal.h stdalign.h stdarg.h stdatomic.h stdbool.h stddef.h stdint.h stdio.h stdlib.h stdnoreturn.h string.h
tgmath.h threads.h time.h uchar.h wchar.h wctype.h
)");
  static const auto posix = words("aio.h arpa/inet.h dirent.h dlfcn.h endian.h fcntl.h fnmatch.h getopt.h glob.h grp.h libgen.h malloc.h netdb.h poll.h pthread.h pwd.h sched.h semaphore.h spawn.h strings.h syslog.h termios.h unistd.h utime.h");
  const auto& header = string(include.at("header"));
  if (standard.contains(header)) return "standard";
  if (posix.contains(header)) return "linux-posix";
  for (const auto prefix : {"sys/", "linux/", "asm/", "net/", "netinet/"}) {
    if (header.starts_with(prefix)) return "linux-posix";
  }
  return {};
}

bool systemCategory(std::string_view category) {
  return category == "standard" || category == "linux-posix";
}



// Splicing precedes comment replacement, as in the JavaScript scan. The
// returned text is solely for recognizing directives; edits retain originals.
std::u16string cleanDirective(std::string_view text) {
  const auto units = cpd::toUtf16(text);
  std::u16string spliced;
  for (std::size_t i = 0; i < units.size(); ++i) {
    if (units[i] == u'\\') {
      if (i + 1 < units.size() && units[i + 1] == u'\n') { ++i; continue; }
      if (i + 2 < units.size() && units[i + 1] == u'\r' && units[i + 2] == u'\n') { i += 2; continue; }
    }
    spliced += units[i];
  }
  std::u16string clean;
  for (std::size_t i = 0; i < spliced.size(); ++i) {
    if (spliced[i] == u'/' && i + 1 < spliced.size() && spliced[i + 1] == u'*') {
      const auto end = spliced.find(u"*/", i + 2);
      if (end != std::u16string::npos) { clean += u' '; i = end + 1; continue; }
    }
    clean += spliced[i];
  }
  while (!clean.empty() && whitespace(clean.back())) clean.pop_back();
  return clean;
}

}  // namespace

std::vector<std::string> commandWords(std::string_view command) {
  const auto units = cpd::toUtf16(command);
  std::vector<std::string> result;
  std::u16string word;
  char16_t quote = 0;
  bool started = false;
  for (std::size_t i = 0; i < units.size(); ++i) {
    const auto ch = units[i];
    if (ch == u'\\' && quote != u'\'') {
      if (++i == units.size()) throw std::runtime_error("unterminated compile-command escape");
      word += units[i];
      started = true;
    } else if (quote && ch == quote) quote = 0;
    else if (!quote && (ch == u'\'' || ch == u'"')) { quote = ch; started = true; }
    else if (!quote && whitespace(ch)) {
      if (started) result.push_back(cpd::fromUtf16(word));
      word.clear();
      started = false;
    } else { word += ch; started = true; }
  }
  if (quote) throw std::runtime_error("unterminated compile-command quote");
  if (started) result.push_back(cpd::fromUtf16(word));
  return result;
}

Json compilationContexts(const Json& entries, std::string_view root,
                         const SourceReader& reader, const PathAvailable& available) {
  Json contexts = Json::object();
  std::map<std::string, Json> policies;
  static const std::regex ownerPattern(R"(^(.*\/CMakeFiles\/([^/]+)\.dir)\/)");
  for (const auto& entry : entries) {
    const auto& directory = string(entry.at("directory"));
    const auto source = resolvePath(directory, string(entry.at("file")));
    const auto file = relativePath(root, source);
    if (file == ".." || file.starts_with("../")) continue;
    const auto arguments = entry.contains("arguments") && !entry.at("arguments").is_null()
        ? entry.at("arguments").get<std::vector<std::string>>() : commandWords(string(entry.at("command")));
    const auto outputIterator = std::find(arguments.begin(), arguments.end(), "-o");
    const bool outputArgument = outputIterator != arguments.end();
    const auto argumentOutput = outputArgument && std::next(outputIterator) != arguments.end()
        ? *std::next(outputIterator) : std::string{};
    const bool metadataOutput = entry.contains("output") && !entry.at("output").is_null();
    const auto output = metadataOutput ? string(entry.at("output")) : argumentOutput;
    if (output.empty()) throw std::runtime_error("compile command has no output: " + file);
    if (metadataOutput && outputArgument && std::next(outputIterator) == arguments.end())
      throw std::runtime_error("compile option -o has no argument: " + file);
    const auto artifact = resolvePath(directory, output);
    const bool outputMismatch = metadataOutput && outputArgument && resolvePath(directory, argumentOutput) != artifact;
    std::smatch owner;
    const bool owned = std::regex_search(artifact, owner, ownerPattern);
    const auto ownerPath = owned ? owner[1].str() : std::string{};
    Json includeDirs = Json::array(), quoteDirs = Json::array(), forced = Json::array();
    for (std::size_t i = 0; i < arguments.size(); ++i) {
      const auto& word = arguments[i];
      if (word == "-I" || word == "-isystem" || word == "-iquote" || word == "-include" ||
          word == "-include-pch" || word == "-imacros") {
        if (i + 1 == arguments.size() || arguments[i + 1].empty())
          throw std::runtime_error("compile option " + word + " has no argument: " + file);
        const auto path = resolvePath(directory, arguments[++i]);
        if (word == "-iquote") quoteDirs.push_back(path);
        else if (word == "-I" || word == "-isystem") includeDirs.push_back(path);
        else forced.push_back({{"option", word}, {"path", path}});
      } else if (word.starts_with("-I")) includeDirs.push_back(resolvePath(directory, word.substr(2)));
      else if (word.starts_with("-isystem")) includeDirs.push_back(resolvePath(directory, word.substr(8)));
      else if (word.starts_with("-iquote")) quoteDirs.push_back(resolvePath(directory, word.substr(7)));
    }
    Json policy = Json::array();
    if (owned) {
      const auto policyPath = ownerPath + "/mmltk-pch-policy.txt";
      if (!policies.contains(policyPath)) {
        Json rows = Json::array();
        if (available ? available(policyPath) : std::filesystem::exists(policyPath)) {
          const auto contents = jsTrim(reader ? reader(policyPath) : read_text(policyPath));
          for (const auto& row : split(contents, '\n')) rows.push_back(split(row, '\t'));
        }
        policies.emplace(policyPath, std::move(rows));
      }
      policy = policies.at(policyPath);
    }
    bool registered = false, excluded = false;
    Json groups = Json::array();
    for (const auto& row : policy) {
      const auto& kind = string(row.at(0));
      if (kind != "use" && kind != "skip" && kind != "header") continue;
      const auto& path = string(row.at(1));
      if (kind == "header") groups.push_back(relativePath(root, path));
      else if (resolvePath(directory, path) == source) {
        if (kind == "use") registered = true;
        else excluded = true;
      }
    }
    const bool modules = std::any_of(arguments.begin(), arguments.end(), [](const auto& word) { return word.starts_with("-fmodule"); });
    Json forcedPch = Json::array();
    for (const auto& item : forced) {
      if (std::filesystem::path(string(item.at("path"))).filename().string().starts_with("cmake_pch.")) forcedPch.push_back(item);
    }
    Json issues = Json::array(), pch = Json::array();
    std::string reason;
    if (artifact.ends_with(".gch")) reason = "PCH artifact creation";
    else {
      if (outputMismatch) issues.push_back("compile output metadata differs from its compiler argument");
      if (registered && excluded) issues.push_back("source is both used and excluded by its target policy");
      if (!forcedPch.empty() && (!owned || !registered)) issues.push_back("forced PCH has no matching target/source registration");
      if (registered && (forcedPch.size() != 1 || forcedPch.at(0).at("option") != "-include" ||
                         forcedPch.at(0).at("path") != ownerPath + "/cmake_pch.hxx"))
        issues.push_back("registered use lacks the exact target-local forced PCH");
      if (registered && groups.empty()) issues.push_back("registered use has no PCH header groups");
      if ((modules || !ordinaryCpp(file)) && !forcedPch.empty()) issues.push_back("excluded language/module compilation forces a PCH");
      if (registered && std::any_of(arguments.begin(), arguments.end(), [](const auto& word) { return word.starts_with("@"); }))
        issues.push_back("response-file arguments hide the complete PCH environment");
      if (!issues.empty()) reason = "inconsistent PCH evidence";
      else if (modules) reason = "module compilation does not consume a PCH";
      else if (!owned) reason = "compile output has no registered CMake owner";
      else if (excluded) reason = "source is excluded by its target PCH policy";
      else if (policy.empty()) reason = "target has no PCH registration";
      else if (!registered) reason = "source is absent from its target PCH consumers";
      else { reason = "target-local forced PCH"; pch = groups; }
    }
    for (auto& item : forcedPch) item["path"] = relativePath(root, string(item.at("path")));
    Json context{{"target", owned ? owner[2].str() : "(unregistered)"}, {"include_dirs", includeDirs}, {"quote_dirs", quoteDirs},
                 {"modules", modules}, {"pch", pch}, {"pch_reason", reason}, {"pch_issues", issues}, {"forced_pch", forcedPch}};
    if (!contexts.contains(file)) contexts[file] = Json::array();
    contexts[file].push_back(std::move(context));
  }
  return contexts;
}

Json scanIncludes(std::string_view source) {
  const auto tokens = cpd::tokenizeCpp(source);
  auto regions = declarations::preprocessorRegions(source, tokens).at("regions");
  const auto lines = declarations::lineStarts(source);
  std::size_t line = 0;
  std::vector<std::size_t> branches;
  Json includes = Json::array();
  static const std::regex plain(R"(^#include (?:<[^>]+>|"[^"]+")\r?$)");
  for (auto& region : regions) {
    const auto clean = cleanDirective(string(region.at("text")));
    std::size_t cursor = 0;
    std::string directive;
    if (!clean.empty() && clean.front() == u'#') {
      cursor = 1;
      while (cursor < clean.size() && whitespace(clean[cursor])) ++cursor;
      const auto begin = cursor;
      while (cursor < clean.size() && text::word(clean[cursor])) ++cursor;
      directive = cpd::fromUtf16(std::u16string_view(clean).substr(begin, cursor - begin));
    }
    if (directive.empty()) region.erase("directive");
    else region["directive"] = directive;
    const auto start = region.at("start").get<std::size_t>();
    if (directive == "if" || directive == "ifdef" || directive == "ifndef") branches.push_back(start);
    if ((directive == "else" || directive == "elif") && !branches.empty()) branches.back() = start;
    if (directive == "endif" && !branches.empty()) branches.pop_back();
    if (directive != "include" && directive != "include_next") continue;
    while (cursor < clean.size() && whitespace(clean[cursor])) ++cursor;
    const auto argument = std::u16string_view(clean).substr(cursor);
    // JavaScript's dot in the directive expression does not span line terminators.
    if (argument.find_first_of(u"\r\n\u2028\u2029") != std::u16string_view::npos) continue;
    std::string header = cpd::fromUtf16(argument), spelling = "macro";
    if (!argument.empty() && (argument.front() == u'<' || argument.front() == u'"')) {
      const auto end = argument.find(argument.front() == u'<' ? u'>' : u'"', 1);
      if (end != std::u16string_view::npos && end > 1) {
        std::size_t tail = end + 1;
        while (tail < argument.size() && whitespace(argument[tail])) ++tail;
        if (tail == argument.size() || argument.substr(tail).starts_with(u"//")) {
          header = cpd::fromUtf16(argument.substr(1, end - 1));
          spelling = argument.front() == u'<' ? "angle" : "quote";
        }
      }
    }
    while (line + 1 < lines.size() && lines[line + 1] <= start) ++line;
    Json branchParts = Json::array();
    for (const auto branch : branches) branchParts.push_back(std::to_string(branch));
    includes.push_back({{"header", header}, {"spelling", spelling}, {"line", line + 1}, {"start", start}, {"end", region.at("end")},
                        {"conditional", !branches.empty()}, {"branch", join(branchParts, ":")},
                        {"plain", std::regex_match(string(region.at("text")), plain) && start == lines.at(line)},
                        {"include_next", directive == "include_next"}});
  }
  Json tokenJson = Json::array();
  for (const auto& token : tokens)
    tokenJson.push_back({{"text", token.text}, {"offset", token.offset}, {"end", token.end}, {"kind", token.kind}});
  return {{"includes", includes}, {"regions", regions}, {"tokens", tokenJson}};
}

Json attributeInclude(const Json& include, std::string_view path, const std::set<std::string>& paths,
                      const Json& contexts, std::string_view root) {
  const auto& spelling = string(include.at("spelling"));
  const auto& header = string(include.at("header"));
  if (spelling == "macro") return {{"category", "macro"}, {"resolution", "unresolved"}};
  Json dirs = Json::array();
  if (spelling == "quote") { dirs.push_back(resolvePath(root, dirname(path))); dirs.push_back(root); }
  for (const auto& context : contexts) {
    if (spelling == "quote") for (const auto& dir : context.at("quote_dirs")) dirs.push_back(dir);
    for (const auto& dir : context.at("include_dirs")) dirs.push_back(dir);
  }
  Json found = Json::array();
  for (const auto& dir : dirs) {
    const auto candidate = relativePath(root, resolvePath(string(dir), header));
    if (paths.contains(candidate)) found.push_back(candidate);
  }
  found = sorted(std::move(found));
  if (found.size() == 1) {
    const auto& candidate = string(found.at(0));
    return {{"category", isPch(candidate) ? "pch" : candidate.starts_with("third_party/") ? "vendored" : "project"},
            {"resolved", candidate}, {"resolution", "include-path"}};
  }
  if (found.size() > 1) return {{"category", "ambiguous"}, {"candidates", found}, {"resolution", "ambiguous"}};
  const auto category = systemHeaderCategory(include);
  if (!category.empty()) return {{"category", category}, {"resolution", "header-name"}};
  if (spelling == "quote") {
    Json suffixes = Json::array();
    for (const auto& candidate : paths) if (candidate.ends_with("/" + header)) suffixes.push_back(candidate);
    if (suffixes.size() == 1) return {{"category", string(suffixes.at(0)).starts_with("third_party/") ? "vendored" : "project"},
                                    {"resolved", suffixes.at(0)}, {"resolution", "unique-suffix"}};
    if (suffixes.size() > 1) return {{"category", "ambiguous"}, {"candidates", sorted(std::move(suffixes))}, {"resolution", "ambiguous"}};
  }
  return {{"category", spelling == "angle" ? "external" : "unresolved"}, {"resolution", "unresolved"}};
}

Json repeatedBundles(const Json& files) {
  Json folders = Json::object(), bundles = Json::array();
  for (const auto& file : files) {
    for (const auto& include : file.at("includes")) {
      if (include.at("category") != "project") continue;
      const auto& header = string(include.at("resolved"));
      const auto folder = dirname(header);
      if (!folders.contains(folder)) folders[folder] = Json::object();
      auto& headers = folders[folder];
      if (!headers.contains(header)) headers[header] = Json::array();
      addUnique(headers[header], file.at("path"));
    }
  }
  for (const auto& [folder, headers] : folders.items()) {
    std::vector<std::pair<std::string, Json>> entries;
    for (const auto& [header, consumers] : headers.items()) {
      if (consumers.size() >= 2) entries.emplace_back(header, consumers);
    }
    std::set<std::string> seen;
    for (std::size_t i = 0; i < entries.size(); ++i) {
      for (std::size_t j = i + 1; j < entries.size(); ++j) {
        Json consumers = Json::array();
        for (const auto& file : entries[i].second) if (has(entries[j].second, string(file))) consumers.push_back(file);
        consumers = sorted(std::move(consumers));
        if (consumers.size() < 2 || !seen.insert(join(consumers, "\n")).second) continue;
        Json shared = Json::array(), external = Json::array(), directories = Json::array();
        for (const auto& [header, uses] : entries) {
          if (std::all_of(consumers.begin(), consumers.end(), [&](const Json& file) { return has(uses, string(file)); })) shared.push_back(header);
        }
        shared = sorted(std::move(shared));
        for (const auto& file : consumers) {
          if (!string(file).starts_with(folder + "/")) {
            external.push_back(file);
            directories.push_back(dirname(string(file)));
          }
        }
        const auto saving = (static_cast<std::int64_t>(shared.size()) - 1) * static_cast<std::int64_t>(consumers.size()) -
                            static_cast<std::int64_t>(shared.size());
        bundles.push_back({{"folder", folder}, {"headers", shared}, {"consumers", consumers}, {"outside_folder", external},
                           {"independent_directories", sorted(std::move(directories))}, {"possible_include_saving", saving}});
      }
    }
  }
  std::stable_sort(bundles.begin(), bundles.end(), [](const Json& a, const Json& b) {
    if (a.at("outside_folder").size() != b.at("outside_folder").size()) return a.at("outside_folder").size() > b.at("outside_folder").size();
    if (a.at("consumers").size() != b.at("consumers").size()) return a.at("consumers").size() > b.at("consumers").size();
    return locale_compare(string(a.at("folder")), string(b.at("folder"))) < 0;
  });
  return bundles;
}

Json pchReplacement(std::string_view path, std::string_view source, const Json& scan,
                    const Json& contexts, const Json& coverage) {
  Json tracked = Json::array(), retained = Json::array();
  for (const auto& include : scan.at("includes")) {
    bool covered = false;
    if (include.at("spelling") != "macro") {
      for (const auto& headers : coverage) if (has(headers, string(include.at("header")))) { covered = true; break; }
    }
    if (!systemHeaderCategory(include).empty() || covered) tracked.push_back(include);
  }
  const auto retain = [&](const Json& include, std::string_view reason) {
    const auto category = systemHeaderCategory(include);
    retained.push_back({{"line", include.at("line")}, {"header", include.at("header")},
                        {"category", category.empty() ? "PCH-covered" : category}, {"reason", reason}});
  };
  static const std::regex declaration(R"(\.(?:h|hh|hpp|hxx|cuh|tpp)(?:\.in)?$)");
  static const std::regex module(R"(\.(?:cppm|ixx)(?:\.in)?$)");
  std::string reason;
  if (isPch(path)) reason = "PCH definition";
  else if (matches(path, declaration)) reason = "declaration header requires direct dependencies";
  else if (path.ends_with(".cu")) reason = "CUDA source does not consume an ordinary C++ PCH";
  else if (matches(path, module)) reason = "module source does not consume a PCH";
  else if (!ordinaryCpp(path)) reason = "not an ordinary C++ translation unit";
  else if (contexts.empty()) reason = "source has no compile command in this build graph";
  else {
    Json exclusions = Json::array();
    for (const auto& context : contexts) {
      if (context.at("modules").get<bool>() || context.at("pch").empty()) {
        exclusions.push_back(context.contains("pch_reason") && !context.at("pch_reason").is_null()
            ? context.at("pch_reason") : Json(context.at("modules").get<bool>()
                ? "module compilation does not consume a PCH" : "compile context has no PCH"));
      }
    }
    if (!exclusions.empty()) reason = join(sorted(std::move(exclusions)), "; ");
  }
  if (!reason.empty()) {
    for (const auto& include : tracked) retain(include, reason);
    return {{"reason", reason}, {"removed", 0}, {"added", 0}, {"retained", retained}};
  }
  Json common = Json::array();
  for (const auto& header : contexts.at(0).at("pch")) {
    if (std::all_of(contexts.begin(), contexts.end(), [&](const Json& context) { return has(context.at("pch"), string(header)); })) common.push_back(header);
  }
  Json replacements = Json::object();
  for (const auto& include : tracked) {
    std::string owner;
    for (const auto& header : common) {
      const auto found = coverage.find(string(header));
      if (found != coverage.end() && has(*found, string(include.at("header")))) { owner = string(header); break; }
    }
    if (owner.empty()) { retain(include, "header is not provided by a PCH shared by every compile context"); continue; }
    if (include.at("spelling") != "angle") { retain(include, "non-angle include retains explicit lookup"); continue; }
    if (include.at("include_next").get<bool>()) { retain(include, "include_next changes lookup"); continue; }
    const auto key = owner + ":" + string(include.at("branch"));
    if (!replacements.contains(key)) replacements[key] = {{"owner", owner}, {"includes", Json::array()}};
    replacements[key]["includes"].push_back(include);
  }
  struct Edit { std::size_t start, end; std::u16string replacement; };
  std::vector<Edit> edits;
  const auto units = cpd::toUtf16(source);
  Json groups = Json::array();
  std::size_t added = 0, removed = 0;
  for (const auto& group : replacements) {
    const auto& header = string(group.at("owner"));
    const auto& includes = group.at("includes");
    bool emitted = std::any_of(scan.at("includes").begin(), scan.at("includes").end(), [&](const Json& include) {
      return include.at("header") == header && include.at("branch") == includes.at(0).at("branch") && !include.at("include_next").get<bool>();
    });
    groups.push_back(header);
    for (const auto& include : includes) {
      const auto start = include.at("start").get<std::size_t>();
      auto end = include.at("end").get<std::size_t>();
      if (end < units.size() && units[end] == u'\n') ++end;
      std::u16string replacement;
      if (!include.at("plain").get<bool>()) {
        replacement = units.substr(start, end - start);
        const auto original = cpd::toUtf16("<" + string(include.at("header")) + ">");
        const auto position = replacement.find(original);
        if (position != std::u16string::npos) replacement.replace(position, original.size(), cpd::toUtf16("\"" + header + "\""));
      } else if (!emitted) {
        replacement = cpd::toUtf16("#include \"" + header + "\"" + (source.find("\r\n") != std::string_view::npos ? "\r\n" : "\n"));
      }
      if (!replacement.empty()) ++added;
      emitted = true;
      edits.push_back({start, end, std::move(replacement)});
      ++removed;
    }
  }
  std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.start > b.start; });
  auto after = units;
  for (const auto& edit : edits) after.replace(edit.start, edit.end - edit.start, edit.replacement);
  const bool compiled = ordinaryCpp(path) && !contexts.empty() && std::all_of(contexts.begin(), contexts.end(), [&](const Json& context) {
    return !context.at("modules").get<bool>() && std::all_of(groups.begin(), groups.end(), [&](const Json& group) { return has(context.at("pch"), string(group)); });
  });
  Json result{{"reason", edits.empty() ? "no covered includes" : compiled ? "compiled PCH" : "textual PCH header"},
              {"removed", removed}, {"added", added}, {"groups", groups}, {"retained", retained}};
  if (!edits.empty()) result["after"] = cpd::fromUtf16(after);
  return result;
}

Json buildIncludeReport(const std::vector<std::string>& paths, const Json& sources,
                        const Json& contexts, std::string_view root) {
  const std::set<std::string> allPaths(paths.begin(), paths.end());
  Json files = Json::array(), scans = Json::object(), frequency = Json::object(), coverage = Json::object();
  for (const auto& [path, sourceJson] : sources.items()) {
    const auto& source = string(sourceJson);
    const auto scan = scanIncludes(source);
    scans[path] = scan;
    if (isPch(path)) {
      coverage[path] = Json::array();
      for (const auto& include : scan.at("includes")) if (include.at("spelling") == "angle") addUnique(coverage[path], include.at("header"));
    }
    const auto& owners = contextsFor(contexts, path);
    Json includes = Json::array(), targets = Json::array(), compileContexts = Json::array();
    for (auto include : scan.at("includes")) {
      include.update(attributeInclude(include, path, allPaths, owners, root));
      includes.push_back(std::move(include));
    }
    for (const auto& owner : owners) {
      targets.push_back(owner.at("target"));
      Json context = Json::object();
      for (const auto* key : {"target", "modules", "pch", "pch_reason", "pch_issues", "forced_pch"}) {
        if (owner.contains(key)) context[key] = owner.at(key);
      }
      compileContexts.push_back(std::move(context));
    }
    std::stable_sort(includes.begin(), includes.end(), [](const Json& a, const Json& b) {
      if (const auto result = locale_compare(string(a.at("category")), string(b.at("category"))); result != 0) return result < 0;
      if (const auto result = locale_compare(string(a.at("header")), string(b.at("header"))); result != 0) return result < 0;
      return a.at("line").get<std::size_t>() < b.at("line").get<std::size_t>();
    });
    const auto lineCount = static_cast<std::size_t>(std::count(source.begin(), source.end(), '\n')) + 1 - (source.ends_with("\n") ? 1 : 0);
    files.push_back({{"path", path}, {"lines", lineCount}, {"targets", sorted(std::move(targets))},
                     {"compile_contexts", compileContexts}, {"includes", includes}});
    if (isPch(path)) continue;
    for (const auto& include : includes) {
      const auto header = include.value("resolved", string(include.at("header")));
      const auto& category = string(include.at("category"));
      const auto key = category + ":" + header;
      if (!frequency.contains(key)) frequency[key] = {{"header", header}, {"category", category}, {"files", Json::array()}, {"occurrences", 0}};
      auto& item = frequency[key];
      addUnique(item["files"], path);
      item["occurrences"] = item.at("occurrences").get<std::size_t>() + 1;
    }
  }
  Json folders = Json::object();
  for (const auto& path : paths) {
    if (path.starts_with("third_party/")) continue;
    const auto dir = dirname(path);
    if (!folders.contains(dir)) folders[dir] = Json::array();
    folders[dir].push_back(path);
  }
  Json directoryReport = Json::array();
  static const std::regex prefixPattern(R"(^([^_.]+)_)");
  for (const auto& [path, entries] : folders.items()) {
    if (entries.size() <= 10) continue;
    Json prefixes = Json::object(), prefixReport = Json::array(), large = Json::array();
    for (const auto& entry : entries) {
      const auto name = std::filesystem::path(string(entry)).filename().string();
      std::smatch match;
      if (std::regex_search(name, match, prefixPattern)) {
        const auto prefix = match[1].str();
        if (!prefixes.contains(prefix)) prefixes[prefix] = Json::array();
        prefixes[prefix].push_back(entry);
      }
    }
    for (const auto& [prefix, members] : prefixes.items()) {
      if (members.size() >= 3) prefixReport.push_back({{"prefix", prefix}, {"files", members}});
    }
    std::stable_sort(prefixReport.begin(), prefixReport.end(), [](const Json& a, const Json& b) { return a.at("files").size() > b.at("files").size(); });
    for (const auto& file : files) {
      if (dirname(string(file.at("path"))) == path && file.at("lines").get<std::size_t>() > 500)
        large.push_back({{"path", file.at("path")}, {"lines", file.at("lines")}});
    }
    // The path list is an inventory and has unique members in normal use;
    // preserve any caller-supplied duplicates in folder listings as JavaScript does.
    auto sortedEntries = entries;
    std::stable_sort(sortedEntries.begin(), sortedEntries.end(), lexicalLess);
    directoryReport.push_back({{"path", path}, {"file_count", entries.size()}, {"files", sortedEntries},
                               {"prefixes", prefixReport}, {"over_500_lines", large}});
  }
  std::stable_sort(directoryReport.begin(), directoryReport.end(), [](const Json& a, const Json& b) {
    if (a.at("file_count") != b.at("file_count")) return a.at("file_count").get<std::size_t>() > b.at("file_count").get<std::size_t>();
    return locale_compare(string(a.at("path")), string(b.at("path"))) < 0;
  });
  auto frequencies = sortedFileRecords(frequency);
  std::stable_sort(frequencies.begin(), frequencies.end(), [](const Json& a, const Json& b) {
    if (a.at("files").size() != b.at("files").size()) return a.at("files").size() > b.at("files").size();
    return locale_compare(string(a.at("header")), string(b.at("header"))) < 0;
  });
  Json replacements = Json::array(), retainedReasons = Json::object();
  for (const auto& file : files) {
    const auto& path = string(file.at("path"));
    Json replacement{{"path", path}};
    replacement.update(pchReplacement(path, string(sources.at(path)), scans.at(path), contextsFor(contexts, path), coverage));
    for (const auto& include : replacement.at("retained")) {
      if (!systemCategory(string(include.at("category"))) || isPch(path)) continue;
      const auto& reason = string(include.at("reason"));
      if (!retainedReasons.contains(reason)) retainedReasons[reason] = {{"reason", reason}, {"files", Json::array()}, {"includes", 0}};
      auto& item = retainedReasons[reason];
      addUnique(item["files"], path);
      item["includes"] = item.at("includes").get<std::size_t>() + 1;
    }
    replacements.push_back(std::move(replacement));
  }
  Json pchCandidates = Json::array(), pchGroups = Json::array();
  for (auto item : frequencies) {
    if (!systemCategory(string(item.at("category"))) || item.at("files").size() < 3) continue;
    item["covered_by"] = Json::array();
    for (const auto& [path, headers] : coverage.items()) if (has(headers, string(item.at("header")))) item["covered_by"].push_back(path);
    pchCandidates.push_back(std::move(item));
  }
  for (const auto& [header, includes] : coverage.items()) {
    Json consumers = Json::array(), compiledConsumers = Json::array();
    for (const auto& file : files) {
      const auto& path = string(file.at("path"));
      if (std::any_of(file.at("includes").begin(), file.at("includes").end(), [&](const Json& entry) {
            return entry.contains("resolved") && entry.at("resolved") == header;
          })) consumers.push_back(path);
      const auto& owners = contextsFor(contexts, path);
      if (ordinaryCpp(path) && !owners.empty() && std::all_of(owners.begin(), owners.end(), [&](const Json& owner) {
            return !owner.at("modules").get<bool>() && has(owner.at("pch"), header);
          })) compiledConsumers.push_back(path);
    }
    pchGroups.push_back({{"header", header}, {"includes", sorted(includes)}, {"consumers", consumers}, {"compiled_consumers", compiledConsumers}});
  }
  auto retainedSystem = sortedFileRecords(retainedReasons);
  std::stable_sort(retainedSystem.begin(), retainedSystem.end(), [](const Json& a, const Json& b) {
    if (a.at("includes") != b.at("includes")) return a.at("includes").get<std::size_t>() > b.at("includes").get<std::size_t>();
    return locale_compare(string(a.at("reason")), string(b.at("reason"))) < 0;
  });
  std::size_t includeCount = 0, changed = 0, removed = 0, added = 0;
  for (const auto& file : files) includeCount += file.at("includes").size();
  for (const auto& file : replacements) {
    changed += file.contains("after") ? 1 : 0;
    removed += file.at("removed").get<std::size_t>();
    added += file.at("added").get<std::size_t>();
  }
  return {{"format", 1}, {"scope", "Git tracked and untracked-unignored first-party files; generated/cache/output and vendored trees excluded. Includes are lexical, including inactive branches. PCH headers do not inflate frequency."},
          {"thresholds", {{"folder_files_exclusive", 10}, {"pch_distinct_files", 3}, {"bundle_consumers", 2}}},
          {"summary", {{"files", paths.size()}, {"include_files", files.size()}, {"includes", includeCount},
                       {"large_folders", directoryReport.size()}, {"changed_files", changed}, {"removed", removed}, {"added", added}}},
          {"folders", directoryReport}, {"pch_candidates", pchCandidates}, {"pch_groups", pchGroups},
          {"retained_system_includes", retainedSystem}, {"frequency", frequencies}, {"bundles", repeatedBundles(files)},
          {"replacements", replacements}, {"files", files}};
}

std::string renderIncludeMarkdown(const Json& report) {
  const auto& summary = report.at("summary");
  std::ostringstream out;
  out << "# Include and organization inventory\n\n" << string(report.at("scope"))
      << "\n\nThis is candidate evidence. Co-inclusion does not prove an umbrella header or folder move is beneficial.\n\n"
      << "Scanned " << summary.at("include_files") << " C/C++/CUDA files and " << summary.at("includes")
      << " directives; " << summary.at("large_folders") << " folders exceed ten files.\n\n"
      << "PCH preview: " << summary.at("changed_files") << " files, " << summary.at("removed")
      << " direct includes replaced by " << summary.at("added") << " PCH includes.\n\n"
      << "## Folders over ten files\n\n| Folder | Files | Prefix families (3+) | C/C++ files over 500 lines |\n|---|---:|---|---|\n";
  for (const auto& folder : report.at("folders")) {
    Json prefixes = Json::array(), large = Json::array();
    for (const auto& group : folder.at("prefixes")) prefixes.push_back(string(group.at("prefix")) + "_ (" + std::to_string(group.at("files").size()) + ")");
    for (const auto& file : folder.at("over_500_lines")) large.push_back(std::filesystem::path(string(file.at("path"))).filename().string() + " (" + file.at("lines").dump() + ")");
    out << "| " << string(folder.at("path")) << " | " << folder.at("file_count") << " | " << join(prefixes, ", ") << " | " << join(large, ", ") << " |\n";
  }
  out << "\n## Standard and Linux/POSIX includes in three or more files\n\n| Header | Category | Files | PCH |\n|---|---|---:|---|\n";
  for (const auto& item : report.at("pch_candidates")) {
    const auto covered = join(item.at("covered_by"), ", ");
    out << "| " << string(item.at("header")) << " | " << string(item.at("category")) << " | " << item.at("files").size()
        << " | " << (covered.empty() ? "candidate" : covered) << " |\n";
  }
  out << "\n## Retained standard and Linux/POSIX includes\n\n"
      << "Every retained directive has a reason in the JSON; compile contexts include forced-PCH evidence and policy contradictions.\n\n"
      << "| Reason | Files | Includes |\n|---|---:|---:|\n";
  for (const auto& item : report.at("retained_system_includes")) {
    out << "| " << string(item.at("reason")) << " | " << item.at("files").size() << " | " << item.at("includes") << " |\n";
  }
  out << "\n## Same-folder bundles\n\nShowing the first 25 of " << report.at("bundles").size()
      << " pair-derived clusters, ranked by consumers outside the folder, then total consumers. All clusters and every include remain in the JSON.\n\n"
      << "Pairs are expanded with the headers shared by all their consumers. This does not enumerate every possible larger subset. Full locations, attribution strength, conditional flags, compiler targets and rewrite reasons are in the JSON.\n";
  std::size_t count = 0;
  for (const auto& bundle : report.at("bundles")) {
    if (count++ == 25) break;
    out << "\n### " << string(bundle.at("folder")) << " (" << bundle.at("consumers").size() << " consumers; "
        << bundle.at("outside_folder").size() << " outside folder)\n\n";
    for (const auto& header : bundle.at("headers")) out << "- " << string(header) << '\n';
    out << "\nConsumers: " << join(bundle.at("consumers"), ", ") << '\n';
  }
  return out.str();
}

Json parseIncludeArgs(const std::vector<std::string>& args) {
  if (args.size() == 1 && (args[0] == "--help" || args[0] == "-h")) return {{"mode", "help"}};
  Json options{{"mode", args.empty() ? "report" : args.front()}, {"output", "output/include-audit"},
               {"build", env("MMLTK_CONTAINER_CACHE_ROOT", ".cache") + "/cmake/release"}};
  if (options.at("mode") != "report" && options.at("mode") != "preview" && options.at("mode") != "fix")
    throw std::runtime_error("expected report, preview or fix; use --help");
  for (std::size_t i = 1; i < args.size(); i += 2) {
    if ((args[i] != "--output" && args[i] != "--build-dir") || i + 1 == args.size() || args[i + 1].empty())
      throw std::runtime_error("expected --output PREFIX or --build-dir PATH");
    options[args[i] == "--output" ? "output" : "build"] = args[i + 1];
  }
  return options;
}

int main(const std::vector<std::string>& args) {
  try {
    const auto options = parseIncludeArgs(args);
    const auto& mode = string(options.at("mode"));
    if (mode == "help") {
      std::cout << "Usage: ./mmltk --audit-includes [--refresh] [report|preview|fix] [--output PREFIX] [--build-dir PATH]\n"
                << "Wrapper --refresh configures the Release graph before scanning; use after changing CMake PCH registrations.\n"
                << "Writes PREFIX.json and PREFIX.md (default output/include-audit). Includes are sorted in the report; source order is preserved.\n"
                << "Fix checks build/toolchain invariants and replaces includes only with a target-local forced PCH in every compile context.\n"
                << "Reports retain every compile context and explain remaining standard/Linux includes and contradictory PCH evidence.\n"
                << "Declaration headers, CUDA, modules and targets without compiled PCHs retain direct includes.\n"
                << "Macro includes and include_next remain explicit. PCH definitions are never rewritten.\n"
                << "PCH additions/registrations and folder/umbrella changes require source review; frequency is candidate evidence.\n";
      return 0;
    }
    const auto root = std::filesystem::current_path().generic_string();
    const auto inventory = cleanup::collectInventoryInputs();
    const auto deletedEntries = inventory.at("deleted").get<std::vector<std::string>>();
    const std::set<std::string> deleted(deletedEntries.begin(), deletedEntries.end());
    auto candidates = inventory.at("tracked");
    for (const auto& path : inventory.at("untracked")) candidates.push_back(path);
    candidates = sorted(std::move(candidates));
    std::vector<std::regex> generated;
    for (const auto& pattern : cleanup::AUTHORED_PROFILE.at("generatedOutputPatterns")) generated.emplace_back(string(pattern));
    std::vector<std::string> paths;
    Json sources = Json::object();
    for (const auto& candidate : candidates) {
      const auto& path = string(candidate);
      if (deleted.contains(path)) continue;
      const auto& prefixes = cleanup::AUTHORED_PROFILE.at("excludedPrefixes");
      if (std::any_of(prefixes.begin(), prefixes.end(), [&](const Json& prefix) { return path.starts_with(string(prefix)); }) ||
          std::any_of(generated.begin(), generated.end(), [&](const std::regex& pattern) { return matches(path, pattern); })) continue;
      if (!std::filesystem::exists(path) || !std::filesystem::is_regular_file(path)) continue;
      paths.push_back(path);
      if (cleanup::authoredLanguage(path) == "cpp") sources[path] = read_text(path);
    }
    const auto& build = string(options.at("build"));
    const auto& output = string(options.at("output"));
    const auto database = build + "/compile_commands.json";
    if (!std::filesystem::exists(database)) throw std::runtime_error("missing " + database + "; run ./mmltk --build or select an existing --build-dir");
    // The invariant checker remains the build authority, as in the original
    // audit. No compile-database command is ever executed by this tool.
    if (mode == "fix") {
      const auto result = run({"python3", "tools/check_toolchain_invariants.py", "--build-dir", build, "--repo-root", root});
      if (result.exit_code != 0) throw std::runtime_error("toolchain invariant check failed: exit " + std::to_string(result.exit_code));
    }
    const auto contexts = compilationContexts(read_json(database), root);
    auto report = buildIncludeReport(paths, sources, contexts, root);
    report["build_directory"] = build;
    report["mode"] = mode;
    cleanup::writeAtomic(output + ".json", report.dump(2) + "\n");
    cleanup::writeAtomic(output + ".md", renderIncludeMarkdown(report));
    if (mode == "fix") {
      for (const auto& [path, source] : sources.items()) {
        if (read_text(path) != string(source)) throw std::runtime_error("source changed during audit: " + path);
      }
      for (const auto& file : report.at("replacements")) {
        if (!file.contains("after")) continue;
        const auto& path = string(file.at("path"));
        const auto permissions = std::filesystem::status(path).permissions();
        cleanup::writeAtomic(path, string(file.at("after")));
        std::filesystem::permissions(path, permissions);
      }
    }
    std::cout << mode << ": " << report.at("summary").dump() << "; " << output << ".{json,md}\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
  }
}

}  // namespace mmltk::tools::include_audit

#if !defined(MMLTK_TOOLS_NO_MAIN) && !defined(MMLTK_AUDIT_INCLUDES_NO_MAIN)
int main(int argc, char** argv) {
  return mmltk::tools::include_audit::main(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
