#pragma once

#include <array>
#include <cstddef>
#include <meta>
#include <type_traits>
#include <tuple>
#include <vector>

#include "src/backend/imaging/explore/explore_render_storage.h"
#include "mmltk/frameworks/reflection/materializer.h"

namespace mmltk::controller::explore_detail {

class GalleryStream;
struct NativeExploreStorageTestAccess;

// This fixed family belongs to GalleryStream. Dynamic read lanes remain
// independently owned and settled by that stream.
class NativeExploreStorage final {
    friend class GalleryStream;
    friend struct NativeExploreStorageTestAccess;
    using Buffer = mmltk::backend::imaging::explore::ExploreHighWaterBuffer;
    using Memory = mmltk::backend::imaging::explore::ExploreBufferMemory;

    struct Family final {
        Buffer cards_device_;
        std::array<Buffer, 2U> cached_clean_;
        std::array<Buffer, 2U> cached_semantic_;
        Buffer annotations_device_;
        Buffer rle_device_;
        Buffer classes_device_;
        Buffer tiles_device_;
        Buffer descriptors_{Memory::PinnedHost};
        Buffer augmented_batch_;
        Buffer donor_boxes_device_;
        Buffer donor_masks_device_;
        Buffer semantic_count_device_;
        Buffer semantic_count_pinned_{Memory::PinnedHost};
    };

    template <auto... Members>
    struct Traversal final {
        template <class Leaf, class Visitor>
        static void VisitLeaf(Leaf& leaf, Visitor& visitor) {
            if constexpr (std::is_same_v<std::remove_cvref_t<Leaf>, Buffer>)
                visitor(leaf);
            else {
                using Array = std::remove_cvref_t<Leaf>;
                static_assert(std::is_same_v<Array, std::array<Buffer, std::tuple_size_v<Array>>>);
                for (auto& buffer : leaf)
                    visitor(buffer);
            }
        }
        template <class Owner, class Visitor>
        static void Visit(Owner& owner, Visitor&& visitor) {
            (VisitLeaf(owner.*Members, visitor), ...);
        }
    };

    struct Materializer final {
        template <class Owner, class Reflection>
        [[nodiscard]] consteval auto operator()() const {
            constexpr auto traversal = [] consteval {
                std::vector<std::meta::info> members;
                template for (constexpr auto member : Reflection::members) members.push_back(std::meta::reflect_constant(&[:member:]));
                return std::meta::substitute(^^Traversal, members);
            }();
            typename[:traversal:] result{};
            return result;
        }
    };

    [[nodiscard]] static consteval auto traversal() { return mmltk::frameworks::reflection::materialize<Family>(Materializer{}); }
    Family buffers_;

   public:
    struct Release final {
        bool all_released = true;
        mmltk::backend::imaging::explore::ExploreStorageStatus failure = 0;
    };
    void Bind(mmltk::backend::imaging::explore::ExploreCudaAllocationApi) noexcept;
    [[nodiscard]] Release ResetChecked() noexcept;
    [[nodiscard]] bool OwnsAllocation() const noexcept;
};

}  // namespace mmltk::controller::explore_detail
