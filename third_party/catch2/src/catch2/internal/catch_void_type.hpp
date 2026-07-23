

#ifndef CATCH_VOID_TYPE_HPP_INCLUDED
#define CATCH_VOID_TYPE_HPP_INCLUDED

namespace Catch {
namespace Detail {

template <typename...>
struct make_void {
    using type = void;
};

template <typename... Ts>
using void_t = typename make_void<Ts...>::type;

}  
}  

#endif  // CATCH_VOID_TYPE_HPP_INCLUDED
