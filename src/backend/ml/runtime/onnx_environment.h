#pragma once
#include <onnxruntime_cxx_api.h>
namespace mmltk::backend::ml::runtime {
// Ordinary backend lifetime owner for the ONNX Runtime process environment.
// Acquisition and explicit release are serialized because ORT deletes its
// singleton outside its own reference-count mutex.
class OnnxEnvironment final {
public:
 OnnxEnvironment(OrtLoggingLevel, const char* log_id);
 ~OnnxEnvironment() noexcept;
 OnnxEnvironment(const OnnxEnvironment&) = delete;
 OnnxEnvironment& operator=(const OnnxEnvironment&) = delete;
 OnnxEnvironment(OnnxEnvironment&&) = delete;
 OnnxEnvironment& operator=(OnnxEnvironment&&) = delete;
 [[nodiscard]] Ort::Env& get() noexcept { return environment_; }

private:
 Ort::Env environment_{nullptr};
};
}  // namespace mmltk::backend::ml::runtime
