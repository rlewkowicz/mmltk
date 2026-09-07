#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <tuple>
#include <type_traits>
#include <utility>

namespace mmltk::common::types {

template <typename Value, auto Member>
using MirroredFieldType = std::remove_cvref_t<decltype(std::declval<const Value&>().*Member)>;

template <typename Value, auto... Members>
class AtomicMirror {
   public:
    void store(const Value& value, const std::memory_order order = std::memory_order_release) noexcept {
        store_fields(value, order, std::make_index_sequence<kFieldCount>{});
    }

    [[nodiscard]] Value load(const std::memory_order order = std::memory_order_acquire) const noexcept {
        return load_fields(order, std::make_index_sequence<kFieldCount>{});
    }

   private:
    static constexpr std::size_t kFieldCount = sizeof...(Members);
    static_assert(kFieldCount > 0U, "an atomic mirror needs at least one mirrored field");

    static constexpr std::tuple<decltype(Members)...> kMembers{Members...};

    using Field = std::tuple_element_t<0U, std::tuple<MirroredFieldType<Value, Members>...>>;
    static_assert((std::is_same_v<Field, MirroredFieldType<Value, Members>> && ...),
                  "every mirrored field must share one atomic element type");
    static_assert(std::atomic<Field>::is_always_lock_free, "mirrored fields must be lock-free atomics");

    template <std::size_t... Index>
    void store_fields(const Value& value, const std::memory_order order, std::index_sequence<Index...> /*indices*/) noexcept {
        (fields_[Index].store(value.*std::get<Index>(kMembers), order), ...);
    }

    template <std::size_t... Index>
    [[nodiscard]] Value load_fields(const std::memory_order order, std::index_sequence<Index...> /*indices*/) const noexcept {
        return Value{fields_[Index].load(order)...};
    }

    std::array<std::atomic<Field>, kFieldCount> fields_{};
};

}  // namespace mmltk::common::types
