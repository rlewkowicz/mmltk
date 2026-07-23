

#include <catch2/internal/catch_stdstreams.hpp>

#include <catch2/catch_user_config.hpp>

#include <iostream>

namespace Catch {

#if !defined(CATCH_CONFIG_NOSTDOUT)
std::ostream& cout() {
    return std::cout;
}
std::ostream& cerr() {
    return std::cerr;
}
std::ostream& clog() {
    return std::clog;
}
#endif

}  
