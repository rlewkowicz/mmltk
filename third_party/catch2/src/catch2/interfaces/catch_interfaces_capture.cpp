

#include <catch2/interfaces/catch_interfaces_capture.hpp>
#include <catch2/internal/catch_enforce.hpp>

namespace Catch {
namespace Detail {
void missingCaptureInstance() {
    CATCH_INTERNAL_ERROR("No result capture instance");
}
}  

IResultCapture::~IResultCapture() = default;
}  
