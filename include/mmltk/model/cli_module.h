#pragma once

#include <string_view>

namespace mmltk::model {

class CliModule {
public:
    virtual ~CliModule() = default;

    [[nodiscard]] virtual std::string_view command_name() const = 0;
    [[nodiscard]] virtual std::string_view summary() const = 0;
    virtual int handle_cli(int argc, char** argv) const = 0;
};

} // namespace mmltk::model
