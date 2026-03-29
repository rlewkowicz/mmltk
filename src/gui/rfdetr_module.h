#pragma once

#include "fastloader/runtime/model_registry.h"

#include <memory>

namespace fastloader::gui {

std::shared_ptr<const fastloader::runtime::ModelModule> make_rfdetr_model_module();

} // namespace fastloader::gui
