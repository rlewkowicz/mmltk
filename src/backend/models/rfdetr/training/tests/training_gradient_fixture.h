#pragma once
#include "src/backend/models/rfdetr/training/detail/training_ops_private.h"
namespace mmltk::backend::models::rfdetr::testsupport {
void exercise_training_initialization(const DistributedContext&, int device);
void exercise_gradient_trajectory(const DistributedContext&, int device);
void exercise_early_bucket_overlap(const DistributedContext&, int device, bool abort_after_launch = false);
}
