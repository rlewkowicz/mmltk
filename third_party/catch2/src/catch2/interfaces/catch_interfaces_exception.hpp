

#ifndef CATCH_INTERFACES_EXCEPTION_HPP_INCLUDED
#define CATCH_INTERFACES_EXCEPTION_HPP_INCLUDED

#include <catch2/internal/catch_unique_ptr.hpp>

#include <string>
#include <vector>

namespace Catch {
using exceptionTranslateFunction = std::string (*)();

class IExceptionTranslator;
using ExceptionTranslators = std::vector<Detail::unique_ptr<IExceptionTranslator const>>;

class IExceptionTranslator {
   public:
    virtual ~IExceptionTranslator();
    virtual std::string translate(ExceptionTranslators::const_iterator it,
                                  ExceptionTranslators::const_iterator itEnd) const = 0;
};

class IExceptionTranslatorRegistry {
   public:
    virtual ~IExceptionTranslatorRegistry();
    virtual std::string translateActiveException() const = 0;
};

}  

#endif  // CATCH_INTERFACES_EXCEPTION_HPP_INCLUDED
