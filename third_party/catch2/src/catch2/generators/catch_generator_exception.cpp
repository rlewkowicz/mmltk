

#include <catch2/generators/catch_generator_exception.hpp>

namespace Catch {

const char* GeneratorException::what() const noexcept {
    return m_msg;
}

}  
