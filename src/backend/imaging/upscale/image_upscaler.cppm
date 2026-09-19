module;
#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include "src/frameworks/gpu/image_failure.h"
#include "upscale_execution.h"
export module mmltk.backend.imaging.upscale.image_upscaler;
export namespace mmltk::backend::imaging::upscale {
enum class ImageUpscalerMode : std::uint8_t {
    Basic,
    ShiftLUT,
    RealPLKSR,
    Count,
};
struct ImageUpscalerModelHandle {
    std::uint64_t id = 0U;
    std::uint64_t version = 0U;
    std::uint64_t backend_generation = 0U;
    std::int32_t device_id = -1;
    [[nodiscard]] bool valid() const noexcept { return id != 0U && version != 0U && backend_generation != 0U && device_id >= 0; }
    auto operator<=>(const ImageUpscalerModelHandle&) const = default;
};
using ImageUpscalerStatus = std::int32_t;
inline constexpr ImageUpscalerStatus kImageUpscalerSuccess = 0;
struct ImageUpscalerAggregateConfig final {
    std::array<ImageUpscalerModelHandle, 3U> models{};
    // Optional effect-only instrumentation, called with an already installed
    // physical owner. Throwing exercises the same checked failure boundary.
    ImageUpscalerExecutionCheckpoint checkpoint{};
    [[nodiscard]] bool valid() const noexcept {
        for (std::size_t index = 0U; index != models.size(); ++index)
            if (!models[index].valid() || models[index].id != index + 1U) return false;
        return true;
    }
};
enum class ImageUpscalerStartError : std::uint8_t {
    InvalidCoordinate,
    DefaultNis,
    ShiftLut,
    RealPlksr,
    Registration,
    Count,
};
class ImageUpscalerProcessOwner;
class ImageUpscaler;
class ImageUpscalerClient final {
   public:
    ImageUpscalerClient() noexcept = default;
    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] ImageUpscalerProcessOwner ClaimOperation() const noexcept;
    [[nodiscard]] std::int32_t device_id() const noexcept { return device_id_; }
    [[nodiscard]] std::uint64_t device_generation() const noexcept { return device_generation_; }
    [[nodiscard]] bool operator==(const ImageUpscalerClient&) const noexcept = default;

   private:
    static constexpr std::uint8_t kInvalidService = std::numeric_limits<std::uint8_t>::max();
    ImageUpscalerClient(std::uint8_t service_slot, std::uint64_t service_generation, std::uint64_t core_generation, std::int32_t device_id,
                        std::uint64_t device_generation) noexcept
        : service_slot_(service_slot),
          service_generation_(service_generation),
          core_generation_(core_generation),
          device_id_(device_id),
          device_generation_(device_generation) {}
    std::uint8_t service_slot_ = kInvalidService;
    std::uint64_t service_generation_ = 0U;
    std::uint64_t core_generation_ = 0U;
    std::int32_t device_id_ = -1;
    std::uint64_t device_generation_ = 0U;
    friend class ImageUpscaler;
};
// This process-local owner is acquired only after a Process record begins
// execution. It holds one exact operation claim and the physical core alive;
// neither it nor any contained resource may cross an invocation boundary.
class ImageUpscalerProcessOwner final {
   public:
    ImageUpscalerProcessOwner() noexcept = default;
    ImageUpscalerProcessOwner(const ImageUpscalerProcessOwner&) = delete;
    ImageUpscalerProcessOwner& operator=(const ImageUpscalerProcessOwner&) = delete;
    ImageUpscalerProcessOwner(ImageUpscalerProcessOwner&&) noexcept;
    ImageUpscalerProcessOwner& operator=(ImageUpscalerProcessOwner&&) noexcept;
    ~ImageUpscalerProcessOwner();
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] ImageUpscalerOutcome run_rgba8(ImageUpscalerModelHandle handle, ImageUpscalerMode mode, const std::uint8_t* source, std::size_t source_pitch,
                                                 std::uint32_t width, std::uint32_t height, std::uint8_t* target, std::size_t target_pitch,
                                                 std::uintptr_t stream, ImageUpscalerCurrent current = image_upscaler_current,
                                                 ImageUpscalerPurpose purpose = ImageUpscalerPurpose::Normal);
    [[nodiscard]] std::uintptr_t operation_stream(ImageUpscalerMode mode, int device_id, ImageUpscalerCurrent current = image_upscaler_current);
    [[nodiscard]] bool graph_replay(ImageUpscalerMode mode) const;

   private:
    explicit ImageUpscalerProcessOwner(std::shared_ptr<void> owner) noexcept;
    void release() noexcept;
    std::shared_ptr<void> owner_{};
    friend class ImageUpscalerClient;
};
class ImageUpscaler final {
   public:
    ~ImageUpscaler();
    [[nodiscard]] static std::expected<std::unique_ptr<ImageUpscaler>, ImageUpscalerStartError> Create(std::int32_t device_id, std::uint64_t device_generation,
                                                                                                       ImageUpscalerAggregateConfig config) noexcept;
    [[nodiscard]] std::expected<void, ImageUpscalerStartError> Activate() noexcept;
    ImageUpscaler(const ImageUpscaler&) = delete;
    ImageUpscaler& operator=(const ImageUpscaler&) = delete;
    ImageUpscaler(ImageUpscaler&&) = delete;
    ImageUpscaler& operator=(ImageUpscaler&&) = delete;
    [[nodiscard]] ImageUpscalerClient client() const noexcept;
    [[nodiscard]] ImageUpscalerStatus Stop() noexcept;
    [[nodiscard]] std::exception_ptr cleanup_failure() const noexcept;
    [[nodiscard]] std::uint64_t core_generation() const noexcept;

   private:
    ImageUpscaler() noexcept = default;
    struct Impl;
    struct Resolver;
    std::shared_ptr<Impl> owner_{};
    ImageUpscalerClient client_{};
    std::uint64_t generation_ = 0U;
    friend class ImageUpscalerClient;
    friend class ImageUpscalerProcessOwner;
};
static_assert(static_cast<std::size_t>(ImageUpscalerMode::Count) == 3U);
static_assert(static_cast<std::size_t>(ImageUpscalerBackend::Count) == 3U);
static_assert(static_cast<std::size_t>(ImageUpscalerStartError::Count) == 5U);
}  // namespace mmltk::backend::imaging::upscale
