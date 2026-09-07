

#ifndef CATCH_MOVE_AND_FORWARD_HPP_INCLUDED
#define CATCH_MOVE_AND_FORWARD_HPP_INCLUDED

#include <type_traits>

#define CATCH_MOVE(...) static_cast<std::remove_reference_t<decltype(__VA_ARGS__)>&&>(__VA_ARGS__)

#define CATCH_FORWARD(...) static_cast<decltype(__VA_ARGS__)&&>(__VA_ARGS__)

#endif  // CATCH_MOVE_AND_FORWARD_HPP_INCLUDED
