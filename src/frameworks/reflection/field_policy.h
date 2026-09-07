#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

// Field policy is host-only application vocabulary.  It deliberately contains
// no UI, persistence, transport, or command-line spelling decisions.
namespace mmltk::frameworks::reflection {

inline constexpr std::size_t kMaximumNameBytes = 256U;
inline constexpr std::size_t kMaximumPathBytes = 4096U;
inline constexpr std::size_t kMaximumCliImageInputs = 4096U;
inline constexpr std::size_t kMaximumTrainingDevices = 16U;
inline constexpr std::size_t kMaximumCpuIdentifiers = 4096U;

[[nodiscard]] inline bool unique_nonnegative_identifiers(const std::span<const int> identifiers) {
    if (std::ranges::any_of(identifiers, [](const int identifier) { return identifier < 0; })) { return false; }
    std::vector<int> ordered(identifiers.begin(), identifiers.end());
    std::ranges::sort(ordered);
    return std::ranges::adjacent_find(ordered) == ordered.end();
}

struct Annotation {
    static constexpr bool is_minimum = false;
    static constexpr bool is_maximum = false;
    static constexpr bool is_finite = false;
    static constexpr bool is_min_bytes = false;
    static constexpr bool is_max_bytes = false;
    static constexpr bool is_max_items = false;
    constexpr bool operator==(const Annotation&) const noexcept = default;
};

template <class Value>
struct Minimum : Annotation {
    Value value;
    static constexpr bool is_minimum = true;
    constexpr explicit Minimum(const Value input) noexcept : value(input) {}
};

template <class Value>
struct Maximum : Annotation {
    Value value;
    static constexpr bool is_maximum = true;
    constexpr explicit Maximum(const Value input) noexcept : value(input) {}
};

struct Finite : Annotation {
    static constexpr bool is_finite = true;
};

struct MinBytes : Annotation {
    std::size_t value;
    static constexpr bool is_min_bytes = true;
    constexpr explicit MinBytes(const std::size_t input) noexcept : value(input) {}
};

struct MaxBytes : Annotation {
    std::size_t value;
    static constexpr bool is_max_bytes = true;
    constexpr explicit MaxBytes(const std::size_t input) noexcept : value(input) {}
};

struct MaxItems : Annotation {
    std::size_t value;
    static constexpr bool is_max_items = true;
    constexpr explicit MaxItems(const std::size_t input) noexcept : value(input) {}
};

enum class FixedTextCharacterPolicy : unsigned char {
    PrintableAscii,
};

struct FixedText : Annotation {
    std::size_t capacity;
    FixedTextCharacterPolicy characters;

    [[nodiscard]] constexpr bool accepts(const std::string_view value) const noexcept {
        if (value.empty() || value.size() > capacity) return false;
        switch (characters) {
            case FixedTextCharacterPolicy::PrintableAscii:
                return std::ranges::all_of(value, [](const unsigned char item) { return item >= 0x20U && item < 0x7fU; });
        }
        return false;
    }
};

enum class PresentationKind : unsigned char {
    Default,
    Preset,
    Path,
    LearningRate,
    Decay,
    Norm,
    UnitInterval,
};

template <PresentationKind Kind>
struct Presentation : Annotation {
    static constexpr PresentationKind kind = Kind;
};

template <class Provider>
struct CatalogProvider : Annotation {
    using provider_type = Provider;
};

template <class>
inline constexpr bool is_catalog_provider_annotation = false;

template <class Provider>
inline constexpr bool is_catalog_provider_annotation<CatalogProvider<Provider>> = true;

enum class Violation : unsigned char {
    InvalidValue,
    NonFinite,
    BelowMinimum,
    AboveMaximum,
    TooFewBytes,
    TooManyBytes,
    TooManyItems,
};

struct ValidationError {
    std::string_view field;
    Violation violation;
};

// A materialized reflected policy.  Reflection adapters construct this once
// from native member annotations; parser, schema, mutation, and persistence
// boundaries consume this value without deriving meaning from field names.
struct FieldConstraint {
    bool finite = false;
    bool has_minimum = false;
    bool has_maximum = false;
    long double minimum = 0.0L;
    long double maximum = 0.0L;
    std::size_t minimum_bytes = 0U;
    std::size_t maximum_bytes = 0U;
    std::size_t maximum_items = 0U;

    constexpr bool operator==(const FieldConstraint&) const noexcept = default;
};

template <class Number>
[[nodiscard]] constexpr bool satisfies(const Number value, const FieldConstraint& constraint) noexcept {
    if constexpr (std::is_floating_point_v<Number>) {
        if (constraint.finite && !std::isfinite(value)) return false;
    }
    const long double numeric = static_cast<long double>(value);
    return (!constraint.has_minimum || numeric >= constraint.minimum) && (!constraint.has_maximum || numeric <= constraint.maximum);
}

template <class Number>
[[nodiscard]] constexpr std::optional<Violation> numeric_violation(const Number value, const FieldConstraint& constraint) noexcept {
    if constexpr (std::is_floating_point_v<Number>) {
        if (constraint.finite && !std::isfinite(value)) return Violation::NonFinite;
    }
    const long double numeric = static_cast<long double>(value);
    if (constraint.has_minimum && numeric < constraint.minimum) return Violation::BelowMinimum;
    if (constraint.has_maximum && numeric > constraint.maximum) return Violation::AboveMaximum;
    return std::nullopt;
}

[[nodiscard]] constexpr bool bounded_text(const std::string_view value, const std::size_t maximum) noexcept {
    return value.size() <= maximum;
}

[[nodiscard]] inline bool satisfies(const std::filesystem::path& value, const FieldConstraint& constraint) noexcept {
    return value.native().size() >= constraint.minimum_bytes &&
           (constraint.maximum_bytes == 0U || value.native().size() <= constraint.maximum_bytes);
}

[[nodiscard]] constexpr bool satisfies(const std::string_view value, const FieldConstraint& constraint) noexcept {
    return value.size() >= constraint.minimum_bytes && (constraint.maximum_bytes == 0U || value.size() <= constraint.maximum_bytes);
}

template <class Container>
[[nodiscard]] constexpr bool satisfies_item_count(const Container& value, const FieldConstraint& constraint) noexcept {
    return constraint.maximum_items == 0U || value.size() <= constraint.maximum_items;
}

}  // namespace mmltk::frameworks::reflection
