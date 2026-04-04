#pragma once

#include "mmltk/rfdetr/train.h"

namespace mmltk::rfdetr::cli_support {

int spawn_distributed_training_workers(const TrainOptions& options, int argc, char** argv);

} // namespace mmltk::rfdetr::cli_support
