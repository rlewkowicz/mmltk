#pragma once

#include "mmltk/rfdetr/validate.h"

namespace mmltk::rfdetr {

class CocoDataset;

ModelInfo inspect_weights_model_info(const ValidationOptions& options);
ValidationBackendResult run_weights_validation_backend(const ValidationOptions& options,
                                                       const CocoDataset& source_dataset);

}  
