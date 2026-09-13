#pragma once
#include "src/controller/contracts/annotation.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mmltk::controller::subsystems::annotation {
struct MaskScratch final {
    std::vector<contracts::AnnotationMaskRun> stroke;
    std::vector<contracts::AnnotationMaskRun> result;
};
// Two-level row index. Render snapshots retain immutable rows; editing copies
// only the index blocks and rows actually touched by a stroke.
class MaskRows final {
    using Runs = std::vector<contracts::AnnotationMaskRun>;
    static constexpr std::size_t kRowsPerBlock = 64U;
    struct Block final {
        std::array<std::shared_ptr<Runs>, kRowsPerBlock> rows;
    };
    struct Index final {
        std::vector<std::shared_ptr<Block>> blocks;
        std::size_t run_count = 0U;
    };

   public:
    void Assign(const contracts::AnnotationObject&);
    void Stroke(contracts::AnnotationPoint, contracts::AnnotationPoint, std::uint16_t radius, std::uint16_t width, std::uint16_t height,
                bool erase, MaskScratch&);
    void Materialize(contracts::AnnotationObject&) const;

   private:
    std::shared_ptr<Index> index_;
};
// Run storage stays normalized: sorted rows, disjoint inclusive intervals.
void normalize_mask(contracts::AnnotationObject&);
void cleanup_mask(contracts::AnnotationObject&, contracts::AnnotationMaskCleanup, std::uint16_t radius, std::uint16_t width,
                  std::uint16_t height);
void transform_mask(contracts::AnnotationObject&, contracts::AnnotationBox, contracts::AnnotationBox, MaskScratch&);
void fill_mask(contracts::AnnotationObject&, contracts::AnnotationPoint, std::uint16_t width, std::uint16_t height);
}  // namespace mmltk::controller::subsystems::annotation
