#pragma once
#include <optional>
#include <string>
#include <string_view>
namespace mmltk::testsupport {
// Process-wide state: callers must exclude concurrent environment changes.
class ScopedEnvironmentVariable final {
public:
 explicit ScopedEnvironmentVariable(std::string_view name, std::optional<std::string_view> value = std::nullopt);
 ~ScopedEnvironmentVariable() noexcept;
 ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) = delete;
 ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) = delete;

private:
 std::string name_;
 std::optional<std::string> previous_;
};
}  // namespace mmltk::testsupport
