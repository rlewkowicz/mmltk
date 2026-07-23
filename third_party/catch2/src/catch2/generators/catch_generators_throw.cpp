

#include <catch2/generators/catch_generator_exception.hpp>
#include <catch2/generators/catch_generators_throw.hpp>
#include <catch2/internal/catch_enforce.hpp>

namespace Catch {
namespace Generators {
namespace Detail {

[[noreturn]]
void throw_generator_exception(char const* msg) {
    Catch::throw_exception(GeneratorException{msg});
}

}  
}  
}  
