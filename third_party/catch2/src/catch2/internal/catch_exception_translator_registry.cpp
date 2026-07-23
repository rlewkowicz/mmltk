

#include <catch2/internal/catch_exception_translator_registry.hpp>
#include <catch2/internal/catch_compiler_capabilities.hpp>
#include <catch2/internal/catch_enforce.hpp>
#include <catch2/internal/catch_test_failure_exception.hpp>
#include <catch2/internal/catch_move_and_forward.hpp>

#include <exception>

namespace Catch {

#if !defined(CATCH_CONFIG_DISABLE_EXCEPTIONS)
namespace {
static std::string tryTranslators(std::vector<Detail::unique_ptr<IExceptionTranslator const>> const& translators) {
    if (translators.empty()) {
        std::rethrow_exception(std::current_exception());
    } else {
        return translators[0]->translate(translators.begin() + 1, translators.end());
    }
}

}  
#endif  //! defined(CATCH_CONFIG_DISABLE_EXCEPTIONS)

ExceptionTranslatorRegistry::~ExceptionTranslatorRegistry() = default;

void ExceptionTranslatorRegistry::registerTranslator(Detail::unique_ptr<IExceptionTranslator>&& translator) {
    m_translators.push_back(CATCH_MOVE(translator));
}

#if !defined(CATCH_CONFIG_DISABLE_EXCEPTIONS)
std::string ExceptionTranslatorRegistry::translateActiveException() const {
    if (std::current_exception() == nullptr) {
        return "Non C++ exception. Possibly a CLR exception.";
    }

    try {
        return tryTranslators(m_translators);
    } catch (TestFailureException&) {
        return "{ nested assertion failed }";
    } catch (TestSkipException&) {
        return "{ nested SKIP() called }";
    } catch (std::exception const& ex) {
        return ex.what();
    } catch (std::string const& msg) {
        return msg;
    } catch (const char* msg) {
        return msg;
    } catch (...) {
        return "Unknown exception";
    }
}

#else  // ^^ Exceptions are enabled // Exceptions are disabled vv
std::string ExceptionTranslatorRegistry::translateActiveException() const {
    CATCH_INTERNAL_ERROR("Attempted to translate active exception under CATCH_CONFIG_DISABLE_EXCEPTIONS!");
}
#endif

}  
