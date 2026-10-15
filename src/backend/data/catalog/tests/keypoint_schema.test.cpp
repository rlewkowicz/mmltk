#include "src/backend/data/catalog/class_catalog.h"
#include "src/backend/data/catalog/keypoint_schema.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
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
TEST_CASE("Named joint correspondence admits shared meaning independently of complete joint sets", "[catalog][keypoint]") {
 using Pairs = std::vector<std::pair<catalog::JointIndex, catalog::JointIndex>>;
 const catalog::ClassKeypointSchema source{{"left", "right", "nose", "old"}, {{0, 2}, {1, 2}, {2, 3}}, {1, 0, 2, 3}, {.05, .05, .07, .08}};
 const catalog::ClassKeypointSchema destination{{"nose", "right", "new", "left"}, {{0, 1}, {3, 0}, {0, 2}}, {0, 3, 2, 1}, {.07, .05, .09, .05}};
 // Edges touching removed/new joints do not change the shared skeleton.
 CHECK(catalog::shared_keypoint_correspondence(source, destination) == Pairs{{2, 0}, {1, 1}, {0, 3}});
 CHECK(catalog::shared_keypoint_correspondence(destination, source) == Pairs{{3, 0}, {1, 1}, {0, 2}});
 CHECK_THROWS(catalog::keypoint_permutation(source, destination));
 const catalog::ClassKeypointSchema disjoint{{"other"}, {}, {0}, {.2}};
 CHECK(catalog::shared_keypoint_correspondence(source, disjoint).empty());
 CHECK(catalog::shared_keypoint_correspondence(source, {}).empty());
 CHECK(catalog::shared_keypoint_correspondence({}, destination).empty());
 CHECK(catalog::keypoint_permutation({}, {}).empty());
 CHECK_THROWS(catalog::keypoint_permutation(source, disjoint));
 CHECK_THROWS(catalog::keypoint_permutation(source, {}));
 auto conflict = destination;
 conflict.oks_sigmas[0] = .2;
 CHECK_THROWS(catalog::shared_keypoint_correspondence(source, conflict));
 conflict = destination;
 conflict.horizontal_flip = {0, 1, 2, 3};
 CHECK_THROWS(catalog::shared_keypoint_correspondence(source, conflict));
 conflict = destination;
 conflict.skeleton.erase(conflict.skeleton.begin());
 CHECK_THROWS(catalog::shared_keypoint_correspondence(source, conflict));
 // A shared joint's named flip partner must agree even if the other joint
 // was removed: restricting the comparison to shared endpoints is invalid.
 const catalog::ClassKeypointSchema paired{{"anchor", "removed"}, {}, {1, 0}, {.1, .1}};
 const catalog::ClassKeypointSchema replacement{{"anchor", "new"}, {}, {1, 0}, {.1, .1}};
 const catalog::ClassKeypointSchema singleton{{"anchor"}, {}, {0}, {.1}};
 CHECK_THROWS(catalog::shared_keypoint_correspondence(paired, replacement));
 CHECK_THROWS(catalog::shared_keypoint_correspondence(paired, singleton));
 // Exact and partial matching use the same checks for a complete named set.
 const std::vector<catalog::JointIndex> permutation{3, 2, 1, 0};
 auto complete = catalog::permute_keypoint_schema(source, permutation);
 CHECK(catalog::keypoint_permutation(source, complete) == permutation);
 CHECK(catalog::shared_keypoint_correspondence(source, complete) == Pairs{{3, 0}, {2, 1}, {1, 2}, {0, 3}});
 for (unsigned kind = 0; kind < 3; ++kind) {
  complete = catalog::permute_keypoint_schema(source, permutation);
  if (kind == 0) complete.oks_sigmas[0] = .2;
  if (kind == 1) complete.horizontal_flip = {0, 1, 2, 3};
  if (kind == 2) complete.skeleton.pop_back();
  CHECK_THROWS(catalog::keypoint_permutation(source, complete));
  CHECK_THROWS(catalog::shared_keypoint_correspondence(source, complete));
 }
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
