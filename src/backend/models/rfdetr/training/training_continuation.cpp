#include "detail/training_continuation.h"
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <meta>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>
#include "archive_utils.h"
#include "detail/checkpoint_private.h"
#include "src/frameworks/serialization/reflected_cbor.h"
namespace mmltk::backend::models::rfdetr {
void validate_resume_continuation_manifest(const ResumeContinuationManifest& manifest) {
    if (manifest.ema_requested != manifest.ema_present) {
        throw std::runtime_error(manifest.ema_requested ? "native RF-DETR resume checkpoint is missing required EMA state"
                                                        : "native RF-DETR resume checkpoint contains EMA state while EMA is disabled");
    }
    if (manifest.scaler_scale.has_value() != manifest.scaler_growth_tracker.has_value()) {
        throw std::runtime_error("native RF-DETR resume checkpoint gradient scaler state is incomplete");
    }
    if ((manifest.scaler_scale.has_value() &&
         (!std::isfinite(*manifest.scaler_scale) || *manifest.scaler_scale <= 0.0 || *manifest.scaler_scale > std::numeric_limits<float>::max())) ||
        (manifest.scaler_growth_tracker.has_value() &&
         (*manifest.scaler_growth_tracker < 0 || *manifest.scaler_growth_tracker > static_cast<int64_t>(std::numeric_limits<int>::max())))) {
        throw std::runtime_error("native RF-DETR resume checkpoint gradient scaler state is invalid");
    }
}
namespace detail {
namespace torch_api = mmltk::backend::ml::torch_api;
namespace serialization = mmltk::frameworks::serialization;
namespace {
// The flat archive contains only this subset of TrainRequest. Its relation to
// native members is declared once; spelling and scalar types derive from them.
inline constexpr std::array kContinuationRequestMembers{^^TrainRequest::optimizer,     ^^TrainRequest::lr_scheduler,    ^^TrainRequest::lr_drop,
                                                        ^^TrainRequest::warmup_epochs, ^^TrainRequest::warmup_momentum, ^^TrainRequest::lr_min_factor};
template <class Visitor>
void visit_request_members(Visitor&& visitor) {
    template for (constexpr auto member : kContinuationRequestMembers) { visitor.template operator()<member>(); }
}
template <class Visitor>
void visit_request_scalars(const TrainRequest& request, Visitor&& visitor) {
    visit_request_members([&]<std::meta::info member> {
        constexpr auto key =
            std::define_static_string(member == ^^TrainRequest::optimizer ? std::string("optimizer_kind") : std::string(std::meta::identifier_of(member)));
        const auto& value = request.[:member:];
        if constexpr (std::is_enum_v<std::remove_cvref_t<decltype(value)>>)
            visitor(key, std::string(cli_enum_spelling(value)));
        else
            visitor(key, value);
    });
}
template <class Visitor>
void visit_augmentation_scalars(const GpuAugmentationConfig& configuration, Visitor&& visitor) {
    template for (constexpr auto member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^GpuAugmentationConfig, std::meta::access_context::current()))) {
        const auto& value = configuration.[:member:];
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(value)>, AugmentationGroupConfig>) {
            template for (constexpr auto field :
                          std::define_static_array(std::meta::nonstatic_data_members_of(^^AugmentationGroupConfig, std::meta::access_context::current()))) {
                constexpr auto key = std::define_static_string(std::string("gpu_augment_") + std::string(std::meta::identifier_of(member)) + "_" +
                                                               std::string(std::meta::identifier_of(field)));
                visitor(key, value.[:field:]);
            }
        } else {
            constexpr auto key = std::define_static_string(std::string("gpu_augment_") + std::string(std::meta::identifier_of(member)));
            visitor(key, value);
        }
    }
}
template <class Values, class Visitor>
void visit_continuation_values(Values& values, Visitor&& visitor) {
    template for (constexpr auto member :
                  std::define_static_array(std::meta::nonstatic_data_members_of(^^TrainingContinuationValues, std::meta::access_context::current()))) {
        visitor(std::define_static_string(std::meta::identifier_of(member)), values.[:member:]);
    }
}
// Convert only the genuinely different archive scalar representation. Comparing
// doubles before conversion back to float avoids accepting rounded forged facts.
template <class T>
auto archive_scalar(const T& value) {
    if constexpr (std::is_floating_point_v<T>)
        return static_cast<double>(value);
    else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
        return static_cast<int64_t>(value);
    else
        return value;
}
template <class T>
void write_scalar(torch_api::OutputArchive& archive, const char* key, const T& value) {
    const auto scalar = archive_scalar(value);
    using Scalar = std::remove_cvref_t<decltype(scalar)>;
    if constexpr (std::is_same_v<Scalar, std::string>)
        write_string(archive, key, scalar);
    else if constexpr (std::is_same_v<Scalar, bool>)
        write_bool(archive, key, scalar);
    else if constexpr (std::is_same_v<Scalar, int64_t>)
        write_int(archive, key, scalar);
    else
        write_double(archive, key, scalar);
}
template <class T>
auto read_scalar(torch_api::InputArchive& archive, const char* key) {
    using Scalar = decltype(archive_scalar(T{}));
    const auto value = read_optional_value<Scalar>(archive, key);
    if (!value) throw std::runtime_error(std::string("full training checkpoint is missing ") + key);
    return *value;
}
void validate_values(const TrainingContinuationValues& values, bool requested_ema, bool present_ema) {
    if (values.epoch < 0 || values.epoch >= std::numeric_limits<int>::max()) throw std::runtime_error("invalid checkpoint epoch");
    if (values.ema_completed_updates < 0 || values.ema_completed_updates == std::numeric_limits<int64_t>::max() ||
        (!requested_ema && values.ema_completed_updates != 0))
        throw std::runtime_error("invalid checkpoint EMA completed update count");
    if (std::isnan(values.best_regular_metric) || std::isnan(values.best_ema_metric)) throw std::runtime_error("checkpoint best metric is invalid");
    if (values.training_attempt_id.empty() || values.training_attempt_id.size() > 64) throw std::runtime_error("invalid checkpoint attempt identity");
    if (values.training_original_descriptor.size() > mmltk::frameworks::reflection::kMaximumPathBytes)
        throw std::runtime_error("invalid original checkpoint descriptor provenance");
    validate_resume_continuation_manifest({requested_ema, present_ema, values.grad_scaler_scale, values.grad_scaler_growth_tracker});
}
bool has_continuation_fields(torch_api::InputArchive& archive) {
    bool present = false;
    const auto probe = [&](const char* key, const auto&...) {
        torch_api::IValue value;
        present = archive.try_read(key, value) || present;
    };
    TrainingContinuationValues values;
    visit_continuation_values(values, probe);
    const TrainRequest request;
    visit_request_scalars(request, probe);
    visit_augmentation_scalars(request.gpu_augmentation, probe);
    probe("optimizer");
    probe("ema_state");
    probe("training_configuration_cbor");
    probe("training_supervision_config_cbor");
    return present;
}
}  // namespace
void write_training_configuration(torch_api::OutputArchive& archive, const TrainRequest& request) {
    constexpr auto capacity = serialization::reflected_maximum_cbor_bytes<TrainRequest>();
    std::vector<std::byte> bytes(capacity);
    const auto encoded = serialization::encode(request, std::span<std::byte>(bytes), {.max_bytes = capacity, .max_items = 4096, .max_depth = 32});
    if (!encoded) throw std::runtime_error("training configuration violates its checkpoint schema");
    auto tensor = torch_api::empty({static_cast<int64_t>(*encoded)}, torch_api::kUInt8);
    std::memcpy(tensor.data_ptr(), bytes.data(), *encoded);
    archive.write("training_configuration_cbor", tensor);
}
TrainRequest read_training_configuration(torch_api::InputArchive& archive) {
    torch_api::Tensor configuration;
    if (!archive.try_read("training_configuration_cbor", configuration))
        throw std::runtime_error("full checkpoint is missing current saved training configuration");
    constexpr auto capacity = serialization::reflected_maximum_cbor_bytes<TrainRequest>();
    if (!configuration.defined() || !configuration.is_cpu() || configuration.scalar_type() != torch_api::kUInt8 || configuration.dim() != 1 ||
        configuration.numel() <= 0 || static_cast<std::size_t>(configuration.numel()) > capacity)
        throw std::runtime_error("invalid checkpoint training configuration");
    configuration = configuration.contiguous();
    const auto request = serialization::decode<TrainRequest>(
        {std::span(reinterpret_cast<const std::byte*>(configuration.const_data_ptr()), static_cast<std::size_t>(configuration.numel())), {}},
        {.max_bytes = capacity, .max_items = 4096, .max_depth = 32});
    if (!request) throw std::runtime_error("invalid checkpoint training configuration values");
    validate_train_request(*request);
    return *request;
}
void write_training_continuation(torch_api::OutputArchive& archive, const TrainRequest& request, const TrainingContinuationValues& values) {
    validate_train_request(request);
    validate_values(values, request.use_ema, request.use_ema);
    write_training_configuration(archive, request);
    write_training_supervision_config(archive, request.training_supervision);
    const auto write = [&](const char* key, const auto& value) { write_scalar(archive, key, value); };
    visit_continuation_values(values, write);
    visit_request_scalars(request, write);
    visit_augmentation_scalars(request.gpu_augmentation, write);
}
std::optional<TrainingContinuation> read_training_continuation(torch_api::InputArchive& archive) {
    torch_api::InputArchive optimizer;
    if (!archive.try_read("optimizer", optimizer)) {
        if (has_continuation_fields(archive)) throw std::runtime_error("full training checkpoint is missing optimizer continuation");
        return std::nullopt;
    }
    TrainingContinuation result;
    result.configuration = read_training_configuration(archive);
    visit_continuation_values(result.values, [&]<class T>(const char* key, T& value) { value = read_scalar<T>(archive, key); });
    const auto require_equal = [&]<class T>(const char* key, const T& value) {
        if (read_scalar<T>(archive, key) != archive_scalar(value))
            throw std::runtime_error(std::string("checkpoint saved configuration disagrees with ") + key);
    };
    visit_request_scalars(result.configuration, require_equal);
    visit_augmentation_scalars(result.configuration.gpu_augmentation, require_equal);
    require_resume_training_supervision_config(archive, result.configuration.training_supervision);
    torch_api::IValue ema_value;
    const bool has_ema = archive.try_read("ema_state", ema_value);
    torch_api::InputArchive ema;
    if (has_ema && !archive.try_read("ema_state", ema)) throw std::runtime_error("checkpoint EMA continuation is not an archive");
    validate_values(result.values, result.configuration.use_ema, has_ema);
    return result;
}
void require_active_training_continuation(const TrainingContinuation& saved, const TrainRequest& active) {
    // Only existing resume compatibility facts restrict an active request.
    // Paths, epoch extension and other historically mutable options stay free.
    visit_request_members([&]<std::meta::info member> {
        constexpr auto name = std::define_static_string(std::meta::identifier_of(member));
        if (saved.configuration.[:member:] != active.[:member:]) throw std::runtime_error(std::string("resume configuration differs for ") + name);
    });
    if (saved.configuration.use_ema != active.use_ema) throw std::runtime_error("resume EMA selection differs from the saved training configuration");
    if (saved.configuration.training_supervision != active.training_supervision)
        throw std::runtime_error("native RF-DETR resume checkpoint training supervision configuration does not match");
}
}  // namespace detail
}  // namespace mmltk::backend::models::rfdetr
