

#include <catch2/internal/catch_startup_exception_registry.hpp>
#include <catch2/internal/catch_compiler_capabilities.hpp>
#include <catch2/catch_user_config.hpp>

namespace Catch {
#if !defined(CATCH_CONFIG_DISABLE_EXCEPTIONS)
void StartupExceptionRegistry::add(std::exception_ptr const& exception) noexcept {
    CATCH_TRY {
        m_exceptions.push_back(exception);
    }
    CATCH_CATCH_ALL {
        std::terminate();
    }
}

std::vector<std::exception_ptr> const& StartupExceptionRegistry::getExceptions() const noexcept {
    return m_exceptions;
}
#endif

}  
