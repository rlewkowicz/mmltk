#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "parse_cli_integer.h"

namespace mmltk::testsupport {

// Table-driven replacement for the hand-rolled `strcmp` chains in the profile runners.
class CliOptionTable {
   public:
    void add_flag(const char* name, bool& target) {
        options_.push_back(Option{name, false, [&target](const char*) { target = true; }});
    }

    void add_string(const char* name, std::string& target) {
        options_.push_back(Option{name, true, [&target](const char* value) { target = value; }});
    }

    void add_integer(const char* name, int& target) {
        options_.push_back(Option{name, true, [name, &target](const char* value) { target = parse_cli_integer(value, name); }});
    }

    // Applies every recognized option; prints `Usage: argv[0] <usage_options>` and exits on any
    // unknown option or missing value, matching the historical hand-rolled parser behavior.
    void parse_or_exit(const int argc, char** argv, const char* usage_options) const {
        for (int index = 1; index < argc; ++index) {
            const Option* matched = find(argv[index]);
            if (matched == nullptr || (matched->takes_value && index + 1 >= argc)) {
                std::fprintf(stderr, "Usage: %s %s\n", argv[0], usage_options);
                std::exit(1);
            }
            matched->apply(matched->takes_value ? argv[++index] : nullptr);
        }
    }

   private:
    struct Option {
        const char* name;
        bool takes_value;
        std::function<void(const char*)> apply;
    };

    [[nodiscard]] const Option* find(const char* argument) const {
        for (const Option& option : options_) {
            if (std::strcmp(option.name, argument) == 0) { return &option; }
        }
        return nullptr;
    }

    std::vector<Option> options_;
};

}  // namespace mmltk::testsupport
