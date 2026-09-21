#include "src/backend/ml/runtime/onnx_environment.h"
#include <mutex>
namespace mmltk::backend::ml::runtime {
namespace {
std::mutex& onnx_environment_mutex() {
 static std::mutex mutex;
 return mutex;
}
}  // namespace
OnnxEnvironment::OnnxEnvironment(const OrtLoggingLevel logging_level, const char* const log_id) {
 std::scoped_lock lock(onnx_environment_mutex());
 environment_ = Ort::Env(logging_level, log_id);
}
OnnxEnvironment::~OnnxEnvironment() noexcept {
 std::scoped_lock lock(onnx_environment_mutex());
 if (OrtEnv* const environment = environment_.release()) Ort::GetApi().ReleaseEnv(environment);
}
}  // namespace mmltk::backend::ml::runtime
