#pragma once
#include <memory>
#include "src/controller/presentation/presentation_system.h"
namespace mmltk::controller::test_support {
// Injects context/import storage while exercising the native source-retirement
// implementation used by Pump and terminal browser cleanup.
struct NativePresentationWriterTestAccess final {
 [[nodiscard]] static std::unique_ptr<PresentationNativeWriter> Create(
  mmltk::frameworks::gpu::DeviceContext, PresentationNativeConfiguration, std::shared_ptr<mmltk::frameworks::gpu::ImageWorkspace>, VisualDiagnosticSink = {});
 static void RetireSource(PresentationNativeWriter&);
};
}  // namespace mmltk::controller::test_support
