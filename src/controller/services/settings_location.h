#pragma once
#include <array>
#include <cstddef>
#include <string_view>
namespace mmltk::controller::services {
// The durable settings location crosses system and worker calls as owned
// bytes. Filesystem objects remain confined to SettingsStore call frames.
class SettingsLocation final {
   public:
    static constexpr std::size_t kCapacity = 512U;
    explicit SettingsLocation(const std::string_view value) noexcept {
        if (value.empty() || value.size() > kCapacity) return;
        for (std::size_t index = 0U; index != value.size(); ++index) bytes_[index] = value[index];
        size_ = value.size();
    }
    [[nodiscard]] bool valid() const noexcept { return size_ != 0U; }
    [[nodiscard]] std::string_view value() const noexcept { return {bytes_.data(), size_}; }

   private:
    std::array<char, kCapacity> bytes_{};
    std::size_t size_ = 0U;
};
}  // namespace mmltk::controller::services
