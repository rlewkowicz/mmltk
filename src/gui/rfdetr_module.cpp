#include "rfdetr_module.h"

namespace mmltk::gui {

std::shared_ptr<const mmltk::model::ModelModule> make_rfdetr_model_module() {
    return mmltk::rfdetr::make_model_module();
}

} // namespace mmltk::gui
