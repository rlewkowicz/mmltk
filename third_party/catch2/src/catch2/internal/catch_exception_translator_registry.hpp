

#ifndef CATCH_EXCEPTION_TRANSLATOR_REGISTRY_HPP_INCLUDED
#define CATCH_EXCEPTION_TRANSLATOR_REGISTRY_HPP_INCLUDED

#include <catch2/interfaces/catch_interfaces_exception.hpp>
#include <catch2/internal/catch_unique_ptr.hpp>

#include <string>

namespace Catch {

class ExceptionTranslatorRegistry : public IExceptionTranslatorRegistry {
   public:
    ~ExceptionTranslatorRegistry() override;
    void registerTranslator(Detail::unique_ptr<IExceptionTranslator>&& translator);
    std::string translateActiveException() const override;

   private:
    ExceptionTranslators m_translators;
};
}  

#endif  // CATCH_EXCEPTION_TRANSLATOR_REGISTRY_HPP_INCLUDED
