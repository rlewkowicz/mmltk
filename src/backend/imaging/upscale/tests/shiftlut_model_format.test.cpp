#include "src/backend/imaging/upscale/detail/shiftlut_model_format.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace lut = mmltk::backend::imaging::upscale::shiftlut;

TEST_CASE("ShiftLUT s7 external blocks preserve the fixed packed format", "[shiftlut_format]") {
    REQUIRE(lut::kTableElements == 222016);
    REQUIRE(lut::kTableBytes == 888064);
    REQUIRE(lut::kTableBlocks == 19);
    REQUIRE(std::string_view(lut::kDomain) == "mmltk.upscale");
    REQUIRE(std::string_view(lut::kOperator) == "ShiftLutS7");
    REQUIRE(lut::kVersion == 1);
    std::size_t end = 0;
    for (std::size_t index = 0; index < lut::kTableBlocks; ++index) {
        const auto block = lut::table_block(index);
        CAPTURE(index);
        REQUIRE(block.offset == end);
        REQUIRE(block.shape.elements() > 0);
        end += block.shape.elements();
        REQUIRE(end <= lut::kTableElements);
    }
    REQUIRE(end == lut::kTableElements);
    const auto low = lut::table_block(0);
    REQUIRE(std::string_view(low.name.data()) == "DW0_LSB");
    REQUIRE(low.shape.outer == 16);
    REQUIRE(low.shape.inner == 9);
    REQUIRE(low.shape.domain == 4);
    for (std::size_t index = 0; index < lut::kStages; ++index) {
        const auto stage = static_cast<lut::Stage>(index);
        const auto dw = lut::table_block(1 + index * 2);
        const auto pw = lut::table_block(2 + index * 2);
        REQUIRE(dw.family == lut::TableFamily::Depthwise);
        REQUIRE(pw.family == lut::TableFamily::Pointwise);
        REQUIRE(dw.stage == stage);
        REQUIRE(pw.stage == stage);
        REQUIRE(std::string_view(dw.name.data()) == "DW" + std::to_string(index) + "_MSB");
        REQUIRE(std::string_view(pw.name.data()) == "PW" + std::to_string(index) + "_MSB");
        REQUIRE(dw.shape.elements() == 9216);
        REQUIRE(pw.shape.elements() == 16384);
        REQUIRE(dw.offset == 576 + index * 25600);
        REQUIRE(pw.offset == dw.offset + dw.shape.elements());
        REQUIRE(lut::table_offset(lut::TableFamily::Depthwise, stage) == dw.offset);
        REQUIRE(lut::table_offset(lut::TableFamily::Pointwise, stage) == pw.offset);
        REQUIRE(lut::table_offset(lut::TableFamily::Shifts, stage) == 221760 + index * 32);
    }
    const auto up = lut::table_block(17);
    const auto shifts = lut::table_block(18);
    REQUIRE(std::string_view(up.name.data()) == "UP_MSB");
    REQUIRE(up.offset == 205376);
    REQUIRE(up.shape.elements() == 16384);
    REQUIRE(std::string_view(shifts.name.data()) == "offset");
    REQUIRE(shifts.shape.outer == 8);
    REQUIRE(shifts.shape.inner == 2);
    REQUIRE(shifts.shape.domain == 16);
    REQUIRE(shifts.offset + shifts.shape.elements() - 1 == 222015);
}

TEST_CASE("ShiftLUT packed storage accepts fractional LUT values and all integral shifts", "[shiftlut_format]") {
    std::vector<float> tables(lut::kTableElements);
    const std::array values{-32767.0F, -0.5F, 0.0F, 0.5F, 32767.0F};
    for (std::size_t index = 0; index < lut::table_offset(lut::TableFamily::Shifts); ++index)
        tables[index] = values[index % values.size()];
    for (std::size_t index = lut::table_offset(lut::TableFamily::Shifts); index < tables.size(); ++index)
        tables[index] = static_cast<float>(static_cast<int>(index % 3) - 1);
    REQUIRE_NOTHROW(lut::validate_tables(std::as_bytes(std::span{tables})));

    // The generator's raw buffer need not be aligned for float access.
    std::vector<std::byte> unaligned(lut::kTableBytes + 1);
    std::memcpy(unaligned.data() + 1, tables.data(), lut::kTableBytes);
    REQUIRE_NOTHROW(lut::validate_tables(std::span{unaligned}.subspan(1)));
    const auto last = tables.back();
    tables.back() = 0;
    std::memcpy(&tables.back(), unaligned.data() + 1 + lut::kTableBytes - sizeof(float), sizeof(float));
    REQUIRE(tables.back() == last);
}

TEST_CASE("ShiftLUT packed storage rejects invalid lengths before scalar access", "[shiftlut_format]") {
    const auto bytes = GENERATE(std::size_t{0}, std::size_t{1}, lut::kTableBytes - 1, lut::kTableBytes + 1);
    const std::vector<std::byte> storage(bytes);
    REQUIRE_THROWS_AS(lut::validate_tables(storage), std::invalid_argument);
}

TEST_CASE("ShiftLUT packed storage rejects nonfinite and excessive LUT magnitudes", "[shiftlut_format]") {
    const auto value = GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity(), -32768.0F, 32768.0F);
    const auto index = GENERATE(std::size_t{0}, lut::table_offset(lut::TableFamily::Shifts) - 1);
    std::vector<float> tables(lut::kTableElements);
    tables[index] = value;
    REQUIRE_THROWS_AS(lut::validate_tables(std::as_bytes(std::span{tables})), std::invalid_argument);
}

TEST_CASE("ShiftLUT packed storage rejects invalid shifts including the last element", "[shiftlut_format]") {
    const auto value = GENERATE(-2.0F, 2.0F, -0.5F, 0.5F, std::numeric_limits<float>::quiet_NaN());
    const auto index = GENERATE(lut::table_offset(lut::TableFamily::Shifts), lut::kTableElements - 1);
    std::vector<float> tables(lut::kTableElements);
    tables[index] = value;
    REQUIRE_THROWS_AS(lut::validate_tables(std::as_bytes(std::span{tables})), std::invalid_argument);
}

TEST_CASE("ShiftLUT tile and decision extents fit fixed resident storage", "[shiftlut_format]") {
    REQUIRE(lut::kMaximumTileExtent == 256);
    REQUIRE(lut::kScratchElements == 12582912);
    REQUIRE(lut::kChannels == 16);
    REQUIRE(lut::kDecisionCheckpoints == 16);
    REQUIRE(lut::decision_elements(0) == 0);
    REQUIRE(lut::resident_bytes(0) == 26053888);
    const auto pixels = GENERATE(std::size_t{1}, std::size_t{1024}, lut::kMaximumTilePixels);
    std::size_t end = 0;
    for (std::size_t index = 0; index < lut::kStages; ++index) {
        for (const auto decision : {lut::Decision::Shifted, lut::Decision::Pointwise}) {
            REQUIRE(lut::decision_offset(static_cast<lut::Stage>(index), decision, pixels) == end);
            end += lut::scratch_elements(pixels);
        }
    }
    REQUIRE(end == lut::decision_elements(pixels));
    REQUIRE(lut::resident_bytes(pixels) - lut::resident_bytes(0) == end);
    REQUIRE(lut::decision_elements(lut::kMaximumTilePixels) == 201326592);
}
