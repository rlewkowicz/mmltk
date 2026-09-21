#include <catch2/catch_test_macros.hpp>
#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/data/catalog/coco_catalog.h"
#include <string>
#include <vector>
namespace catalog = mmltk::backend::data::catalog;
TEST_CASE("Catalog preserves ordered foreground identity and exact names", "[catalog]") {
 const catalog::ClassCatalog first({"background", "person", "toothbrush"});
 const catalog::ClassCatalog reordered({"toothbrush", "background", "person"});
 CHECK(first.resolve("background") == 0U);
 CHECK_FALSE(first.resolve("Person"));
 CHECK_FALSE(first.ordered_equal(reordered));
 CHECK(first.permutation_to(reordered) == std::vector<std::uint32_t>{1, 2, 0});
 CHECK_THROWS(first.permutation_to(catalog::ClassCatalog({"background", "person", "car"})));
 CHECK(catalog::ClassCatalog{}.empty());
 CHECK(catalog::ClassCatalog(first.record()).ordered_equal(first));
}
TEST_CASE("Catalog rejects ambiguous or unrepresentable names", "[catalog]") {
 CHECK_THROWS(catalog::ClassCatalog({""}));
 CHECK_THROWS(catalog::ClassCatalog({std::string("a\0b", 3)}));
 CHECK_THROWS(catalog::ClassCatalog({"person", "person"}));
 CHECK_THROWS(catalog::ClassCatalog({std::string(257, 'x')}));
 CHECK_NOTHROW(catalog::ClassCatalog({std::string(31, 'x')}, 31));
 CHECK_THROWS(catalog::ClassCatalog({std::string(32, 'x')}, 31));
 CHECK_NOTHROW(catalog::ClassCatalog({std::string(30, 'x') + "a"}, 31));
 CHECK_THROWS(catalog::ClassCatalog({std::string(30, 'x') + "é"}, 31));
 std::vector<std::string> names;
 for (unsigned index = 0; index < 256; ++index) names.push_back("class-" + std::to_string(index));
 const catalog::ClassCatalog full(names);
 CHECK(full.resolve("class-255") == 255U);
 names.push_back("overflow");
 CHECK_THROWS(catalog::ClassCatalog(names));
}
TEST_CASE("COCO source IDs retain their sparse external domain", "[catalog]") {
 CHECK(catalog::coco_foreground_index(1) == 0U);
 CHECK(catalog::coco_foreground_index(13) == 11U);
 CHECK(catalog::coco_foreground_index(90) == 79U);
 CHECK_FALSE(catalog::coco_foreground_index(0));
 CHECK_FALSE(catalog::coco_foreground_index(12));
 CHECK(catalog::kCocoCategories[79].source_id == 90U);
 CHECK(catalog::kCocoNames[11] == "stop sign");
}
