

#include <catch2/internal/catch_uncaught_exceptions.hpp>
#include <catch2/internal/catch_config_uncaught_exceptions.hpp>
#include <catch2/catch_user_config.hpp>

#include <exception>

namespace Catch {
bool uncaught_exceptions() {
#if defined(CATCH_CONFIG_DISABLE_EXCEPTIONS)
    return false;
#elif defined(CATCH_CONFIG_CPP17_UNCAUGHT_EXCEPTIONS)
    return std::uncaught_exceptions() > 0;
#else
    return std::uncaught_exception();
#endif
}
}  
