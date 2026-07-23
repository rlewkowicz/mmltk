#pragma once

#include "mmltk/rfdetr/module.h"

#include <memory>

namespace mmltk::gui {

std::shared_ptr<const mmltk::model::ModelModule> make_rfdetr_model_module();

}
