

#ifndef CATCH_COMPLETE_INVOKE_HPP_INCLUDED
#define CATCH_COMPLETE_INVOKE_HPP_INCLUDED

#include <catch2/internal/catch_meta.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

namespace Catch {
namespace Benchmark {
namespace Detail {
template <typename T>
struct CompleteType {
    using type = T;
};
template <>
struct CompleteType<void> {
    struct type {};
};

template <typename T>
using CompleteType_t = typename CompleteType<T>::type;

template <typename Result>
struct CompleteInvoker {
    template <typename Fun, typename... Args>
    static Result invoke(Fun&& fun, Args&&... args) {
        return CATCH_FORWARD(fun)(CATCH_FORWARD(args)...);
    }
};
template <>
struct CompleteInvoker<void> {
    template <typename Fun, typename... Args>
    static CompleteType_t<void> invoke(Fun&& fun, Args&&... args) {
        CATCH_FORWARD(fun)(CATCH_FORWARD(args)...);
        return {};
    }
};

template <typename Fun, typename... Args>
CompleteType_t<FunctionReturnType<Fun, Args...>> complete_invoke(Fun&& fun, Args&&... args) {
    return CompleteInvoker<FunctionReturnType<Fun, Args...>>::invoke(CATCH_FORWARD(fun), CATCH_FORWARD(args)...);
}

}  

template <typename Fun>
Detail::CompleteType_t<FunctionReturnType<Fun>> user_code(Fun&& fun) {
    return Detail::complete_invoke(CATCH_FORWARD(fun));
}
}  
}  

#endif  // CATCH_COMPLETE_INVOKE_HPP_INCLUDED
