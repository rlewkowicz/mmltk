#pragma once

#include "src/frameworks/reflection/field_policy.h"
#include "src/frameworks/reflection/reflected_field_policy.h"

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <meta>
#include <optional>
#include <string_view>

#include "mmltk/frameworks/reflection/materializer.h"

namespace mmltk::backend::models::rfdetr {

struct RfdetrCommandText final {
    static constexpr std::size_t kCapacity = 64U;

    char value[kCapacity]{};
    std::uint8_t size = 0U;

    consteval RfdetrCommandText() = default;

    template <std::size_t Size>
    consteval RfdetrCommandText(const char (&text)[Size]) : size(static_cast<std::uint8_t>(Size - 1U)) {
        static_assert(Size > 0U && Size - 1U <= kCapacity);
        for (std::size_t index = 0U; index + 1U < Size; ++index)
            value[index] = text[index];
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept { return {value, size}; }
};

struct RfdetrCommandDeclaration final {
    RfdetrCommandText name;
    RfdetrCommandText alias_a;
    RfdetrCommandText alias_b;
    RfdetrCommandText description;
};

enum class RfdetrCommand : std::uint8_t {
    Compile[[= RfdetrCommandDeclaration{"compile", {}, {}, "Compile datasets for RF-DETR workflows"}]],
    Info[[= RfdetrCommandDeclaration{"info", {}, {}, "Inspect an RF-DETR model artifact"}]],
    BuildEngine[[= RfdetrCommandDeclaration{"build-engine", {}, {}, "Build a TensorRT engine from an ONNX model"}]],
    ExportOnnx[[= RfdetrCommandDeclaration{"export-onnx", {}, {}, "Export RF-DETR weights to ONNX"}]],
    Predict[[= RfdetrCommandDeclaration{"predict", {}, {}, "Run RF-DETR prediction"}]],
    Evaluate[[= RfdetrCommandDeclaration{"evaluate", "eval", "val", "Evaluate an RF-DETR model"}]],
    Validate[[= RfdetrCommandDeclaration{"validate", {}, {}, "Validate an RF-DETR model"}]],
    Train[[= RfdetrCommandDeclaration{"train", {}, {}, "Train an RF-DETR model"}]],
    NormalizeWeights[[= RfdetrCommandDeclaration{"normalize-weights", {}, {}, "Normalize an upstream RF-DETR checkpoint"}]],
};

struct RfdetrCommandDescriptor final {
    RfdetrCommand command;
    std::string_view name;
    std::string_view alias_a;
    std::string_view alias_b;
    std::string_view description;

    constexpr bool operator==(const RfdetrCommandDescriptor&) const noexcept = default;
};

struct RfdetrCommandMaterializer final {
    template <class Enum, class Reflection>
    [[nodiscard]] consteval auto operator()() const {
        std::array<RfdetrCommandDescriptor, Reflection::enumerators.size()> result{};
        std::size_t index = 0U;
        template for (constexpr auto enumerator : Reflection::enumerators) {
            RfdetrCommandDeclaration declaration{};
            std::size_t declaration_count = 0U;
            mmltk::frameworks::reflection::visit_annotations<enumerator>([&]<class Annotation>(const Annotation& annotation) {
                if constexpr (std::same_as<Annotation, RfdetrCommandDeclaration>) {
                    declaration = annotation;
                    ++declaration_count;
                }
            });
            if (declaration_count != 1U) throw "each RF-DETR command requires one declaration";
            result[index++] = {
                .command = [:enumerator:],
                                         .name = std::define_static_string(declaration.name.view()),
                                         .alias_a = std::define_static_string(declaration.alias_a.view()),
                                         .alias_b = std::define_static_string(declaration.alias_b.view()),
                                         .description = std::define_static_string(declaration.description.view()),
            };
        }
        return result;
    }
};

inline constexpr auto kRfdetrCommands = mmltk::frameworks::reflection::materialize<RfdetrCommand>(RfdetrCommandMaterializer{});

[[nodiscard]] constexpr const RfdetrCommandDescriptor* rfdetr_command_descriptor(const RfdetrCommand command) noexcept {
    for (const auto& descriptor : kRfdetrCommands) {
        if (descriptor.command == command) return &descriptor;
    }
    return nullptr;
}

[[nodiscard]] constexpr std::optional<RfdetrCommand> parse_rfdetr_command(const std::string_view spelling) noexcept {
    for (const auto& descriptor : kRfdetrCommands) {
        if (spelling == descriptor.name || (!descriptor.alias_a.empty() && spelling == descriptor.alias_a) ||
            (!descriptor.alias_b.empty() && spelling == descriptor.alias_b)) {
            return descriptor.command;
        }
    }
    return std::nullopt;
}

[[nodiscard]] consteval bool rfdetr_command_vocabulary_is_valid() {
    for (std::size_t left = 0U; left < kRfdetrCommands.size(); ++left) {
        const auto& descriptor = kRfdetrCommands[left];
        if (descriptor.name.empty() || (!descriptor.alias_a.empty() && descriptor.alias_a == descriptor.name) ||
            (!descriptor.alias_b.empty() && (descriptor.alias_b == descriptor.name || descriptor.alias_b == descriptor.alias_a)) ||
            rfdetr_command_descriptor(descriptor.command) != &descriptor) {
            return false;
        }
        for (std::size_t right = left + 1U; right < kRfdetrCommands.size(); ++right) {
            const auto& candidate = kRfdetrCommands[right];
            const auto matches = [&candidate](const std::string_view value) {
                return !value.empty() && (value == candidate.name || value == candidate.alias_a || value == candidate.alias_b);
            };
            if (matches(descriptor.name) || matches(descriptor.alias_a) || matches(descriptor.alias_b)) { return false; }
        }
    }
    return true;
}

static_assert(rfdetr_command_vocabulary_is_valid());

}  // namespace mmltk::backend::models::rfdetr
