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
template<class Family>
class GalleryBufferFamily final {
    using Buffer = mmltk::backend::imaging::explore::ExploreHighWaterBuffer;
    template <auto... Members>
    struct Traversal final {
        template <class Leaf, class Visitor>
        static void VisitLeaf(Leaf& leaf, Visitor& visitor) {
            if constexpr (std::is_same_v<std::remove_cvref_t<Leaf>, Buffer>)
                visitor(leaf);
            else {
                using Array = std::remove_cvref_t<Leaf>;
                static_assert(std::is_same_v<Array, std::array<Buffer, std::tuple_size_v<Array>>>);
                for (auto& buffer : leaf) visitor(buffer);
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
public:
    Family buffers_;
    struct Release final {
        bool all_released = true;
        mmltk::backend::imaging::explore::ExploreStorageStatus failure = 0;
    };
    template<class Visitor> void Visit(Visitor&& visitor) { traversal().Visit(buffers_, visitor); }
    template<class Visitor> void Visit(Visitor&& visitor) const { traversal().Visit(buffers_, visitor); }
    void Bind(mmltk::backend::imaging::explore::ExploreCudaAllocationApi api) noexcept { Visit([api](auto& buffer) { buffer.bind(api); }); }
    [[nodiscard]] Release ResetChecked() noexcept {
        Release result;
        Visit([&result](auto& buffer) {
            const auto status = buffer.reset();
            if (result.failure == 0) result.failure = status;
        });
        result.all_released = !OwnsAllocation();
        return result;
    }
    [[nodiscard]] bool OwnsAllocation() const noexcept {
        bool owned = false;
        Visit([&owned](const auto& buffer) { owned |= buffer.owns_allocation(); });
        return owned;
    }
};
}
