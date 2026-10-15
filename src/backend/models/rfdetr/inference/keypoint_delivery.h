#pragma once
#include <torch/types.h>
#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/models/rfdetr/core/postprocess.h"
namespace mmltk::backend::models::rfdetr {
// The caller supplies reusable storage; every write remains on its inference stream.
void write_analysis_keypoints(const PostprocessedSelection&, mmltk::backend::ml::runtime::AnalysisAnnotationStorage&);
}  // namespace mmltk::backend::models::rfdetr
