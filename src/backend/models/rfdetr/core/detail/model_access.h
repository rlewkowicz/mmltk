#pragma once
#include <type_traits>
#include "model_technical.h"
namespace mmltk::backend::models::rfdetr::detail {
template <class Model>
[[nodiscard]] inline decltype(auto) native_model_owner(Model& model) noexcept {
    using Owner = std::conditional_t<std::is_const_v<Model>, const NativeModelTechnicalOwner, NativeModelTechnicalOwner>;
    return *static_cast<Owner*>(model.technical_handle());
}
}  // namespace mmltk::backend::models::rfdetr::detail
