#include "src/controller/subsystems/system/detail/prediction_sampling.h"
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <limits>
#include <set>
using namespace mmltk::controller::detail;
TEST_CASE("prediction subsets preserve exact cardinality with smaller-set storage", "[prediction][sampling]") {
 std::mt19937_64 random(73);
 for (std::uint64_t count = 1; count <= 101; ++count) {
  PredictionSelection selection(101,count,random);
  std::uint64_t accepted = 0;
  for (std::uint64_t index = 0; index < 101; ++index) accepted += selection.Contains(index);
  CHECK(accepted == count);
  CHECK(selection.retained_indices() == std::min(count,101-count));
 }
 CHECK(PredictionSelection::Percent(101,10) == 11);
 CHECK(PredictionSelection::Percent(std::numeric_limits<std::uint64_t>::max(),100) == std::numeric_limits<std::uint64_t>::max());
 CHECK_THROWS(PredictionSelection(0,1,random));
 CHECK_THROWS(PredictionSelection(5,6,random));
 CHECK_THROWS(PredictionSelection::Percent(10,0));
}
TEST_CASE("video reservoir observes actual population and bounded distinct slots", "[prediction][sampling]") {
 std::mt19937_64 random(19), replay(19);
 PredictionReservoir reservoir(6), same(6);
 std::array<std::uint64_t,6> frames{};
 for (std::uint64_t index = 0; index < 1000; ++index) {
  const auto slot = reservoir.Observe(random);
  REQUIRE(slot == same.Observe(replay));
  if (slot) { REQUIRE(*slot < frames.size()); frames[*slot] = index; }
 }
 CHECK(reservoir.observed() == 1000);
 CHECK(reservoir.selected() == 6);
 CHECK(std::set(frames.begin(),frames.end()).size() == 6);
 PredictionReservoir short_source(100);
 for (unsigned index = 0; index < 3; ++index) CHECK(short_source.Observe(random) == index);
 CHECK(short_source.selected() == 3);
 CHECK_THROWS(PredictionReservoir(0));
}

TEST_CASE("compiled saving counts use full source and eligible inference populations independently", "[prediction][sampling]") {
 const auto count=PredictionSelection::Percent(100,10);
 CHECK(count==10U);
 CHECK(PredictionSelection::Eligible(100,10,count)==10U);
 CHECK(PredictionSelection::Eligible(100,0,count)==100U);
 CHECK_THROWS_WITH(PredictionSelection::Eligible(100,9,count),"prediction sample count exceeds the images eligible under the inference limit");
 std::mt19937_64 random(1);
 PredictionSelection selected(PredictionSelection::Eligible(100,10,count),count,random);
 for (unsigned ordinal=0;ordinal<10;++ordinal) CHECK(selected.Contains(ordinal));
}
