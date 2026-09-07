#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

namespace mmltk::frameworks::gpu {

template <typename State, typename Releaser>
    requires std::default_initializable<State> && std::movable<State> && std::default_initializable<Releaser> &&
             std::invocable<Releaser&, State&>
class UniqueResource {
    static_assert(std::is_nothrow_default_constructible_v<State>);
    static_assert(std::is_nothrow_move_constructible_v<State>);
    static_assert(std::is_nothrow_move_assignable_v<State>);
    static_assert(std::is_nothrow_invocable_v<Releaser&, State&>);

   public:
    UniqueResource() = default;
    explicit UniqueResource(State state) noexcept(std::is_nothrow_move_constructible_v<State>) : state_(std::move(state)) {}
    UniqueResource(const UniqueResource&) = delete;
    UniqueResource& operator=(const UniqueResource&) = delete;

    UniqueResource(UniqueResource&& other) noexcept(std::is_nothrow_move_constructible_v<State>)
        : state_(std::exchange(other.state_, State{})) {}

    UniqueResource& operator=(UniqueResource&& other) noexcept(std::is_nothrow_move_assignable_v<State>) {
        if (this != &other) { reset(std::exchange(other.state_, State{})); }
        return *this;
    }

    ~UniqueResource() { reset(); }

    void reset(State replacement = {}) noexcept {
        Releaser{}(state_);
        state_ = std::move(replacement);
    }

    [[nodiscard]] State& state() noexcept { return state_; }

    [[nodiscard]] const State& state() const noexcept { return state_; }

   private:
    State state_{};
};

}  // namespace mmltk::frameworks::gpu
