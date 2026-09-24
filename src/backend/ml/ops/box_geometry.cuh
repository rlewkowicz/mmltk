#pragma once
#include <cuda_runtime.h>
#include <cmath>
namespace mmltk::backend::ml::ops::box_geometry {
template<class T> __device__ T minimum(T a, T b) { return isnan(a) || a < b ? a : b; }
template<class T> __device__ T maximum(T a, T b) { return isnan(a) || a > b ? a : b; }
template<class T> __device__ T area(const T* box) { return (box[2] - box[0]) * (box[3] - box[1]); }
template<class T> struct Pair {
 T intersection, union_area, enclosure;
 __device__ T iou() const { return intersection / maximum(union_area, static_cast<T>(1e-7)); }
 __device__ T giou() const { return iou() - (enclosure - union_area) / maximum(enclosure, static_cast<T>(1e-7)); }
};
template<class T> __device__ Pair<T> pair(const T* a, const T* b) {
 const T intersection = maximum(T{0}, minimum(a[2], b[2]) - maximum(a[0], b[0])) * maximum(T{0}, minimum(a[3], b[3]) - maximum(a[1], b[1]));
 const T enclosure = maximum(T{0}, maximum(a[2], b[2]) - minimum(a[0], b[0])) * maximum(T{0}, maximum(a[3], b[3]) - minimum(a[1], b[1]));
 return {intersection, area(a) + area(b) - intersection, enclosure};
}
template<class T> __device__ void corners(const T* box, T* result) {
 const T w = maximum(box[2], T{0}) * T{0.5};
 const T h = maximum(box[3], T{0}) * T{0.5};
 result[0] = box[0] - w; result[1] = box[1] - h;
 result[2] = box[0] + w; result[3] = box[1] + h;
}
}
