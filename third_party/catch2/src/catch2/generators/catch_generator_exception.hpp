

#ifndef CATCH_GENERATOR_EXCEPTION_HPP_INCLUDED
#define CATCH_GENERATOR_EXCEPTION_HPP_INCLUDED

#include <exception>

namespace Catch {

class GeneratorException : public std::exception {
    const char* const m_msg = "";

   public:
    GeneratorException(const char* msg) : m_msg(msg) {}

    const char* what() const noexcept final;
};

}  

#endif  // CATCH_GENERATOR_EXCEPTION_HPP_INCLUDED
