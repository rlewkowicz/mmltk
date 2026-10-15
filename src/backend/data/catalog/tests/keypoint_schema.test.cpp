#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/data/catalog/keypoint_schema.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>
#include <numeric>
#include <string>
#include <vector>
namespace catalog = mmltk::backend::data::catalog;
TEST_CASE("Keypoint schema preserves named joint meaning across permutations", "[catalog][keypoint]") {
 const auto person = catalog::coco_person_keypoint_schema();
 REQUIRE(person.names.size() == 17U);
 REQUIRE_NOTHROW(catalog::validate_keypoint_schema(person));
 std::vector<catalog::JointIndex> permutation(person.names.size());
 std::iota(permutation.rbegin(), permutation.rend(), catalog::JointIndex{0});
 const auto reordered = catalog::permute_keypoint_schema(person, permutation);
 CHECK(catalog::keypoint_permutation(person, reordered) == permutation);
 CHECK(reordered.names[permutation[5]] == "left_shoulder");
 CHECK(reordered.horizontal_flip[permutation[5]] == permutation[6]);
 CHECK(reordered.oks_sigmas[permutation[5]] == person.oks_sigmas[5]);
 catalog::OrderedClassCatalog source{{{"person"}, {"car"}}, std::vector<catalog::ClassKeypointSchema>{person, {}}};
 catalog::OrderedClassCatalog target{{{"car"}, {"person"}}, std::vector<catalog::ClassKeypointSchema>{{}, reordered}};
 const catalog::ClassCatalog admitted(source), destination(target);
 CHECK(admitted.permutation_to(destination) == std::vector<std::uint32_t>{1U, 0U});
 CHECK(admitted.has_keypoints());
 CHECK(admitted.keypoints(1).names.empty());
 CHECK(catalog::ClassCatalog(admitted.record()).ordered_equal(admitted));
 target.keypoints->at(1).oks_sigmas[0] *= 2.0;
 CHECK_THROWS(admitted.permutation_to(catalog::ClassCatalog(target)));
 source.keypoints->pop_back();
 CHECK_THROWS(catalog::ClassCatalog(source));
 CHECK_FALSE(catalog::ClassCatalog({"car"}).has_keypoints());
 CHECK_NOTHROW(catalog::validate_keypoint_schema({}));
}
TEST_CASE("Keypoint schema admission rejects malformed identity and geometry metadata", "[catalog][keypoint]") {
 const auto person = catalog::coco_person_keypoint_schema();
 auto invalid = person;
 invalid.names[1] = invalid.names[0];
 CHECK_THROWS(catalog::validate_keypoint_schema(invalid));
 invalid = person;
 invalid.names[0] = std::string(catalog::kKeypointNameCapacity + 1U, 'x');
 CHECK_THROWS(catalog::validate_keypoint_schema(invalid));
 invalid = person;
 invalid.names[0] = std::string("a\0b", 3U);
 CHECK_THROWS(catalog::validate_keypoint_schema(invalid));
 invalid = person;
 invalid.horizontal_flip[1] = 1U;
 CHECK_THROWS(catalog::validate_keypoint_schema(invalid));
 invalid = person;
 invalid.horizontal_flip.pop_back();
 CHECK_THROWS(catalog::validate_keypoint_schema(invalid));
 for (const auto sigma : {0.0, -0.1, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
  invalid = person;
  invalid.oks_sigmas[0] = sigma;
  CHECK_THROWS(catalog::validate_keypoint_schema(invalid));
 }
 invalid = person;
 invalid.skeleton.push_back({13U, 15U});
 CHECK_THROWS(catalog::validate_keypoint_schema(invalid));
 invalid = person;
 invalid.skeleton[0].first = 17U;
 CHECK_THROWS(catalog::validate_keypoint_schema(invalid));
 invalid = person;
 invalid.skeleton[0].second = invalid.skeleton[0].first;
 CHECK_THROWS(catalog::validate_keypoint_schema(invalid));
 CHECK_THROWS(catalog::permute_keypoint_schema(person, std::vector<catalog::JointIndex>(17U, 0U)));
 CHECK(catalog::checked_joint_index(255U, 256U) == 255U);
 CHECK_THROWS(catalog::checked_joint_index(256U, 256U));
 CHECK_THROWS(catalog::checked_joint_index(0U, 0U));
 CHECK(catalog::checked_keypoint_payload_count(30U, 17U) == 510U);
 CHECK(catalog::checked_keypoint_payload_count(std::numeric_limits<std::size_t>::max(), 0U) == 0U);
 CHECK_THROWS(catalog::checked_keypoint_payload_count(std::numeric_limits<std::size_t>::max(), 17U));
 CHECK_THROWS(catalog::checked_keypoint_payload_count(1U, catalog::kKeypointsPerClassCapacity + 1U));
 CHECK(catalog::checked_foreground_index(255U) == 255U);
 CHECK_THROWS(catalog::checked_foreground_index(256U));
}
TEST_CASE("Keypoint annotation keeps visible occluded and unannotated labels distinct", "[catalog][keypoint]") {
 CHECK(static_cast<std::uint8_t>(catalog::KeypointVisibility::Unannotated) == 0U);
 CHECK(static_cast<std::uint8_t>(catalog::KeypointVisibility::Occluded) == 1U);
 CHECK(static_cast<std::uint8_t>(catalog::KeypointVisibility::Visible) == 2U);
 std::vector<catalog::KeypointAnnotation> joints(3U);
 joints[0].visibility = catalog::KeypointVisibility::Visible;
 joints[1].visibility = catalog::KeypointVisibility::Occluded;
 REQUIRE_NOTHROW(catalog::validate_keypoint_annotations(joints, 3U));
 CHECK_THROWS(catalog::validate_keypoint_annotations(joints, 2U));
 joints[0].x = std::numeric_limits<float>::quiet_NaN();
 CHECK_THROWS(catalog::validate_keypoint_annotations(joints, 3U));
 joints[0].x = 0.0F;
 joints[0].visibility = static_cast<catalog::KeypointVisibility>(3U);
 CHECK_THROWS(catalog::validate_keypoint_annotations(joints, 3U));
}
