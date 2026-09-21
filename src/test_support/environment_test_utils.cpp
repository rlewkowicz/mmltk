#include "src/test_support/environment_test_utils.hpp"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
namespace mmltk::testsupport {
ScopedEnvironmentVariable::ScopedEnvironmentVariable(const std::string_view name, const std::optional<std::string_view> value) : name_(name) {
    if (const char* existing = std::getenv(name_.c_str()); existing != nullptr) previous_.emplace(existing);
    const int result = value ? ::setenv(name_.c_str(), std::string{*value}.c_str(), 1) : ::unsetenv(name_.c_str());
    if (result != 0) throw std::runtime_error("cannot change test environment variable " + name_ + ": " + std::strerror(errno));
}
ScopedEnvironmentVariable::~ScopedEnvironmentVariable() noexcept {
    if (previous_)
        static_cast<void>(::setenv(name_.c_str(), previous_->c_str(), 1));
    else
        static_cast<void>(::unsetenv(name_.c_str()));
}
}  // namespace mmltk::testsupport
