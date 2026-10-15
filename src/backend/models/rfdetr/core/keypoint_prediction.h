#pragma once
namespace mmltk::backend::models::rfdetr {
// Pinned GroupPose channel order. This scalar declaration is also CUDA-usable:
// channel offsets are offsetof(KeypointPrediction, member) / sizeof(float),
// and the packet width is sizeof(KeypointPrediction) / sizeof(float).
// Findability/visibility remain logits; precision is raw Cholesky parameterization.
struct KeypointPrediction final {
 float x = 0.0F;
 float y = 0.0F;
 float findable_logit = 0.0F;
 float visible_logit = 0.0F;
 float log_l11 = 0.0F;
 float l21 = 0.0F;
 float log_l22 = 0.0F;
 float class_logit = 0.0F;
};
}  // namespace mmltk::backend::models::rfdetr
