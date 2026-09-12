#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "src/controller/contracts/annotation.h"

namespace mmltk::controller {
struct AnnotationEdit;
struct AnnotationPointer;
struct AnnotationRenderState;
}  // namespace mmltk::controller

namespace mmltk::controller::subsystems::annotation {

namespace domain = mmltk::controller::contracts;

enum class DocumentOutcome : std::uint8_t { Applied, Rejected, Capacity };

struct DocumentResult final {
    DocumentOutcome outcome = DocumentOutcome::Rejected;
    std::string detail;
    bool render_changed = false;
};

enum class DocumentSaveEffect : std::uint8_t { NotApplied, Committed, Uncertain };
[[nodiscard]] DocumentSaveEffect save_annotation_document(const contracts::AnnotationUiState&, std::string_view, std::uint64_t) noexcept;

class AnnotationDocument final {
   public:
    AnnotationDocument();
    ~AnnotationDocument();
    AnnotationDocument(const AnnotationDocument&) = delete;
    AnnotationDocument& operator=(const AnnotationDocument&) = delete;

    [[nodiscard]] DocumentResult Open(contracts::AnnotationSceneContent);
    [[nodiscard]] DocumentResult Pointer(const mmltk::controller::AnnotationPointer&);
    // Translate an exact displayed target into the current document's indices.
    // The reducer itself continues to operate on ordinary current-index targets.
    [[nodiscard]] bool ResolveTarget(mmltk::controller::AnnotationPointer&) const noexcept;
    bool PeerClosed() noexcept;
    [[nodiscard]] DocumentResult Edit(const mmltk::controller::AnnotationEdit&);
    [[nodiscard]] DocumentResult Save(std::string_view);
    [[nodiscard]] const contracts::AnnotationUiState& ui() const noexcept;
    [[nodiscard]] bool ToolAvailable(contracts::AnnotationTool, std::optional<std::uint16_t>) const noexcept;
    // At most three descriptions are retained by the input/pending/render owners.
    void CaptureRender(AnnotationRenderState&);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mmltk::controller::subsystems::annotation
