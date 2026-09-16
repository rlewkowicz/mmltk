#pragma once
#include "model_state_technical.h"
namespace mmltk::backend::models::rfdetr::detail {
template <class State>
[[nodiscard]] inline ModelStateTechnicalOwner& model_state_owner(State& state) noexcept {
    return *static_cast<ModelStateTechnicalOwner*>(state.technical_handle());
}
template <class State>
[[nodiscard]] inline const ModelStateTechnicalOwner& model_state_owner(const State& state) noexcept {
    return *static_cast<const ModelStateTechnicalOwner*>(state.technical_handle());
}
}  // namespace mmltk::backend::models::rfdetr::detail
