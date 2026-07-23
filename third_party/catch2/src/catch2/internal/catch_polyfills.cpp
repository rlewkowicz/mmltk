

#include <catch2/internal/catch_polyfills.hpp>
#include <catch2/internal/catch_compiler_capabilities.hpp>
#include <catch2/catch_user_config.hpp>

#include <cmath>

namespace Catch {

#if !defined(CATCH_CONFIG_POLYFILL_ISNAN)
bool isnan(float f) {
    return std::isnan(f);
}
bool isnan(double d) {
    return std::isnan(d);
}
#else
bool isnan(float f) {
    return std::_isnan(f);
}
bool isnan(double d) {
    return std::_isnan(d);
}
#endif

#if !defined(CATCH_CONFIG_GLOBAL_NEXTAFTER)
float nextafter(float x, float y) {
    return std::nextafter(x, y);
}
double nextafter(double x, double y) {
    return std::nextafter(x, y);
}
#else
float nextafter(float x, float y) {
    return ::nextafterf(x, y);
}
double nextafter(double x, double y) {
    return ::nextafter(x, y);
}
#endif

}  
