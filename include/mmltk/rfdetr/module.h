#pragma once

#include "mmltk/model/cli_module.h"
#include "mmltk/model/module.h"

#include <memory>

namespace mmltk::rfdetr {

std::shared_ptr<const mmltk::model::ModelModule> make_model_module();
const mmltk::model::CliModule& cli_module();

}  // namespace mmltk::rfdetr
