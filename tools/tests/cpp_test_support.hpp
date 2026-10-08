#pragma once

#include "../cpp_support.hpp"

#include <exception>
#include <functional>
#include <iostream>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace mmltk::tools::testing {

class Suite {
public:
    bool add(const char* name, void (*run)()) {
        cases_.push_back({name, run});
        return true;
    }
    int run() const {
        std::size_t failed = 0;
        for (const auto& test : cases_) {
            try { test.run(); }
            catch (const std::exception& error) {
                ++failed;
                std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
            }
        }
        std::cout << cases_.size() - failed << '/' << cases_.size() << " cases passed\n";
        return failed == 0 ? 0 : 1;
    }
private:
    struct Case { const char* name; void (*run)(); };
    std::vector<Case> cases_;
};

inline void require(bool condition, std::string_view expression, int line) {
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + std::string(expression));
}

enum class ErrorMatch { regex, substring };
inline void throws(const std::function<void()>& operation, std::string_view pattern = {},
                   ErrorMatch mode = ErrorMatch::regex) {
    try { operation(); }
    catch (const std::exception& error) {
        const bool matches = pattern.empty() || (mode == ErrorMatch::regex
            ? std::regex_search(error.what(), std::regex(std::string(pattern)))
            : std::string_view(error.what()).find(pattern) != std::string_view::npos);
        if (!matches) throw std::runtime_error("unexpected error: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("expected an exception");
}

inline json operator""_fixture(const char* source, std::size_t length) {
    return json::parse(std::string_view(source, length));
}

inline std::vector<std::string> detector_paths(const std::vector<std::string>& args) {
    const auto option = std::find(args.begin(), args.end(), "--file-list");
    if (option == args.end() || std::next(option) == args.end()) throw std::runtime_error("missing file list");
    std::ifstream list(*std::next(option));
    if (!list) throw std::runtime_error("cannot read detector file list");
    std::vector<std::string> result;
    std::string path;
    while (std::getline(list, path)) if (!path.empty()) result.push_back(path);
    return result;
}

} // namespace mmltk::tools::testing

#define TEST_CASE(name, id) \
    void id(); \
    const bool register_##id = suite.add(name, id); \
    void id()
#define CHECK(...) ::mmltk::tools::testing::require((__VA_ARGS__), #__VA_ARGS__, __LINE__)
