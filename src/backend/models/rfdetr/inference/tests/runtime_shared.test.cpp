#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <type_traits>
#include <utility>

#include "src/backend/ml/runtime/analysis_provider.h"
#include "src/backend/ml/runtime/backend_factory.h"
#include "src/backend/ml/runtime/tensorrt_runtime.h"
#include "src/backend/models/rfdetr/inference/evaluation.h"
#include "src/backend/models/rfdetr/inference/validate.h"

import mmltk.backend.models.rfdetr.inference.analysis_provider;
import mmltk.backend.models.rfdetr.inference.prediction;
import mmltk.backend.models.rfdetr.inference.runtime_backend;

TEST_CASE("RF-DETR analysis provider implements the neutral analysis contract", "[model][rfdetr][analysis_provider]") {
    using mmltk::backend::ml::runtime::AnalysisProvider;
    using mmltk::backend::models::rfdetr::MakeRfdetrAnalysisProvider;
    using mmltk::backend::models::rfdetr::RfdetrAnalysisOptions;

    static_assert(std::is_same_v<decltype(MakeRfdetrAnalysisProvider(std::declval<const RfdetrAnalysisOptions&>())),
                                 std::shared_ptr<AnalysisProvider>>);
    SUCCEED();
}
