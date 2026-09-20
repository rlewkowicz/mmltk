#pragma once  // backend.data private implementation boundary
#include "benchmark_cache.h"
#include "coconut_catalog.h"
#include "src/frameworks/reflection/reflected_field_policy.h"
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
namespace mmltk::backend::data::benchmark_internal {
struct CoconutPhysicalImage {
    CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
    std::uint64_t image_id = 0;
    std::uint16_t shard = 0;
    std::string member;
    std::string archive_identity;
    bool operator==(const CoconutPhysicalImage&) const = default;
};
MMLTK_REFLECT_FIELDS(CoconutPhysicalImage)
struct CoconutInventoryImage {
    CoconutPhysicalImage physical;
    std::uint64_t release_image_id = 0;
    std::uint64_t source_ordinal = 0;
    bool operator==(const CoconutInventoryImage&) const = default;
};
MMLTK_REFLECT_FIELDS(CoconutInventoryImage)
struct InventoryHeader {
    std::uint64_t magic = 0x314E564954554E43ULL;
    std::uint32_t version = 1;
    std::uint32_t cache_schema = kBenchmarkCacheSchemaVersion;
    std::string normalization = std::string(kCoconutNormalizationRevision);
    std::string input_identity;
    CoconutEdition edition = CoconutEdition::Base;
    CoconutImageNamespace source = CoconutImageNamespace::CocoTrain;
    std::uint16_t shard = 0;
    bool component = false;
    std::uint64_t count = 0;
};
MMLTK_REFLECT_FIELDS(InventoryHeader)
// CNUTIVN1 is a declaration-order little-endian format, not a native struct dump.
// These fixed expectations guard version 1; they do not drive any codec.
static_assert(CHAR_BIT == 8 && sizeof(bool) == 1 && sizeof(std::uint8_t) == 1 &&
              sizeof(std::uint16_t) == 2 && sizeof(std::uint32_t) == 4 && sizeof(std::uint64_t) == 8);
static_assert(std::is_same_v<std::underlying_type_t<CoconutEdition>, std::uint8_t>);
static_assert(std::is_same_v<std::underlying_type_t<CoconutImageNamespace>, std::uint8_t>);
static_assert([] consteval {
    const auto matches = []<class Record, class... Types>(
        std::type_identity<std::tuple<Types...>>, const std::array<std::string_view, sizeof...(Types)>& names) consteval {
        constexpr const auto& fields = mmltk::frameworks::reflection::field_declarations<Record>();
        using Fields = std::remove_cvref_t<decltype(fields)>;
        static_assert(fields.size() == sizeof...(Types));
        Fields::Visit([]<class Declaration, std::size_t Index>() {
            static_assert(std::is_same_v<typename Declaration::member_type, std::tuple_element_t<Index, std::tuple<Types...>>>);
        });
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (fields[index].member_name != names[index]) return false;
        }
        return true;
    };
    return matches.template operator()<CoconutPhysicalImage>(
               std::type_identity<std::tuple<CoconutImageNamespace, std::uint64_t, std::uint16_t, std::string, std::string>>{},
               {"source", "image_id", "shard", "member", "archive_identity"}) &&
           matches.template operator()<CoconutInventoryImage>(
               std::type_identity<std::tuple<CoconutPhysicalImage, std::uint64_t, std::uint64_t>>{},
               {"physical", "release_image_id", "source_ordinal"}) &&
           matches.template operator()<InventoryHeader>(
               std::type_identity<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::string, std::string,
                                            CoconutEdition, CoconutImageNamespace, std::uint16_t, bool, std::uint64_t>>{},
               {"magic", "version", "cache_schema", "normalization", "input_identity", "edition", "source", "shard", "component", "count"});
}());
}  // namespace mmltk::backend::data::benchmark_internal
