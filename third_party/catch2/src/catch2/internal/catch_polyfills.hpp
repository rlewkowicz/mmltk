

#ifndef CATCH_POLYFILLS_HPP_INCLUDED
#define CATCH_POLYFILLS_HPP_INCLUDED

namespace Catch {

bool isnan(float f);
bool isnan(double d);

float nextafter(float x, float y);
double nextafter(double x, double y);

}  

#endif  // CATCH_POLYFILLS_HPP_INCLUDED
