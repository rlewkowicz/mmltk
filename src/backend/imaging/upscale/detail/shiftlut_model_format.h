#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#if defined(__CUDACC__)
#define MMLTK_SHIFTLUT_FORMAT_HD __host__ __device__
#else
#define MMLTK_SHIFTLUT_FORMAT_HD
#endif

namespace mmltk::backend::imaging::upscale::shiftlut {

inline constexpr const char* kDomain = "mmltk.upscale";
inline constexpr const char* kOperator = "ShiftLutS7";
inline constexpr int kVersion = 1;
inline constexpr int kChannels = 16;
inline constexpr int kRgbChannels = 3;
inline constexpr int kRotations = 4;
inline constexpr int kRotatedBatch = kRgbChannels * kRotations;
inline constexpr int kScale = 4;
inline constexpr int kOutputPhases = kScale * kScale;
inline constexpr int kMaximumTileExtent = 256;
inline constexpr int kSpatialTaps = 9;
inline constexpr int kLowDomain = 4;
inline constexpr int kHighDomain = 64;
inline constexpr int kShiftAxes = 2;
enum class Stage : std::uint8_t { First, Second, Third, Fourth, Fifth, Sixth, Seventh, Eighth };
inline constexpr std::size_t kStages = static_cast<std::size_t>(Stage::Eighth) + 1;
enum class TableFamily : std::uint8_t { Low, Depthwise, Pointwise, Up, Shifts };
enum class Decision : std::uint8_t { Shifted, Pointwise };
inline constexpr std::size_t kDecisionsPerStage = 2;
inline constexpr std::size_t kDecisionCheckpoints = kStages * kDecisionsPerStage;

struct Dimensions {
    std::size_t outer;
    std::size_t inner;
    std::size_t domain;
    MMLTK_SHIFTLUT_FORMAT_HD constexpr std::size_t elements() const noexcept { return outer * inner * domain; }
};

MMLTK_SHIFTLUT_FORMAT_HD constexpr Dimensions dimensions(TableFamily family) noexcept {
    switch (family) {
        case TableFamily::Low: return {kChannels, kSpatialTaps, kLowDomain};
        case TableFamily::Depthwise: return {kChannels, kSpatialTaps, kHighDomain};
        case TableFamily::Pointwise: return {kChannels, kChannels, kHighDomain};
        case TableFamily::Up: return {kOutputPhases, kChannels, kHighDomain};
        case TableFamily::Shifts: return {kStages, kShiftAxes, kChannels};
    }
    return {};
}

MMLTK_SHIFTLUT_FORMAT_HD constexpr std::size_t table_offset(TableFamily family, Stage stage = Stage::First) noexcept {
    const auto low = dimensions(TableFamily::Low).elements();
    const auto depthwise = dimensions(TableFamily::Depthwise).elements();
    const auto pair = depthwise + dimensions(TableFamily::Pointwise).elements();
    switch (family) {
        case TableFamily::Low: return 0;
        case TableFamily::Depthwise: return low + static_cast<std::size_t>(stage) * pair;
        case TableFamily::Pointwise: return low + static_cast<std::size_t>(stage) * pair + depthwise;
        case TableFamily::Up: return low + kStages * pair;
        case TableFamily::Shifts: return low + kStages * pair + dimensions(TableFamily::Up).elements() +
            static_cast<std::size_t>(stage) * kShiftAxes * kChannels;
    }
    return 0;
}

// The ordered external inventory is a projection of the typed stage/family layout.
struct TableBlock {
    TableFamily family;
    Stage stage;
    std::array<char, 8> name{};
    Dimensions shape;
    std::size_t offset;
};
inline constexpr std::size_t kTableBlocks = 1 + kStages * 2 + 2;
constexpr TableBlock table_block(std::size_t index) noexcept {
    TableFamily family = TableFamily::Low;
    Stage stage = Stage::First;
    if (index == kTableBlocks - 1) family = TableFamily::Shifts;
    else if (index == kTableBlocks - 2) family = TableFamily::Up;
    else if (index != 0) {
        family = (index - 1) % 2 == 0 ? TableFamily::Depthwise : TableFamily::Pointwise;
        stage = static_cast<Stage>((index - 1) / 2);
    }
    TableBlock result{family, stage, {}, dimensions(family), table_offset(family, stage)};
    if (family == TableFamily::Shifts) {
        result.name = {'o', 'f', 'f', 's', 'e', 't'};
    } else if (family == TableFamily::Up) {
        result.name = {'U', 'P', '_', 'M', 'S', 'B'};
    } else {
        result.name = {family == TableFamily::Pointwise ? 'P' : 'D', 'W',
                       static_cast<char>('0' + static_cast<char>(stage)), '_',
                       family == TableFamily::Low ? 'L' : 'M', 'S', 'B'};
    }
    return result;
}

inline constexpr std::size_t kTableElements =
    table_offset(TableFamily::Shifts) + dimensions(TableFamily::Shifts).elements();
inline constexpr std::size_t kTableBytes = kTableElements * sizeof(float);
inline constexpr std::size_t kMaximumTilePixels = kMaximumTileExtent * kMaximumTileExtent;
constexpr std::size_t scratch_elements(std::size_t pixels) noexcept { return kRotatedBatch * kChannels * pixels; }
inline constexpr std::size_t kScratchElements = scratch_elements(kMaximumTilePixels);
constexpr std::size_t decision_elements(std::size_t pixels) noexcept { return kDecisionCheckpoints * scratch_elements(pixels); }
constexpr std::size_t decision_offset(Stage stage, Decision decision, std::size_t pixels) noexcept {
    return (static_cast<std::size_t>(stage) * kDecisionsPerStage + static_cast<std::size_t>(decision)) * scratch_elements(pixels);
}
inline constexpr std::size_t kDecisionStorageOffset = 2 * kScratchElements;
constexpr std::size_t resident_bytes(std::size_t decision_capacity) noexcept {
    return kTableBytes + (kDecisionStorageOffset + decision_elements(decision_capacity)) * sizeof(std::int8_t);
}

// Cold-path validation accepts unaligned little-endian FP32 storage.
void validate_tables(std::span<const std::byte> bytes);

}  // namespace mmltk::backend::imaging::upscale::shiftlut

#undef MMLTK_SHIFTLUT_FORMAT_HD
