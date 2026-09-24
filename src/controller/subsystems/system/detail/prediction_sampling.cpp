#include "prediction_sampling.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
namespace mmltk::controller::detail {
std::uint64_t PredictionSelection::Percent(std::uint64_t population, unsigned percent) {
 if (!population || !percent || percent > 100U) throw std::invalid_argument("prediction sampling requires a nonempty population and percent in [1,100]");
 return (population / 100U) * percent + ((population % 100U) * percent + 99U) / 100U;
}
std::uint64_t PredictionSelection::Eligible(std::uint64_t population, std::uint64_t limit, std::uint64_t count) {
 const auto eligible=limit ? std::min(population,limit) : population;
 if (!count || count>eligible) throw std::invalid_argument("prediction sample count exceeds the images eligible under the inference limit");
 return eligible;
}
PredictionSelection::PredictionSelection(std::uint64_t population, std::uint64_t count, std::mt19937_64& random)
 : population_(population), excluded_(count > population / 2U) {
 if (!population || !count || count > population) throw std::invalid_argument("prediction sample count is outside the admitted population");
 const auto stored = std::min(count, population - count);
 if (stored > indices_.max_size()) throw std::length_error("prediction selection exceeds index capacity");
 indices_.reserve(static_cast<std::size_t>(stored));
 for (auto index = population - stored; index < population; ++index) {
  const auto selected = std::uniform_int_distribution<std::uint64_t>(0, index)(random);
  if (!indices_.insert(selected).second) indices_.insert(index);
 }
}
bool PredictionSelection::Contains(std::uint64_t index) const {
 if (index >= population_) throw std::out_of_range("prediction index exceeds its admitted population");
 return indices_.contains(index) != excluded_;
}
PredictionReservoir::PredictionReservoir(std::uint64_t capacity) : capacity_(capacity) {
 if (!capacity) throw std::invalid_argument("video sample count must be positive");
}
std::optional<std::uint64_t> PredictionReservoir::Observe(std::mt19937_64& random) {
 if (observed_ == std::numeric_limits<std::uint64_t>::max()) throw std::overflow_error("decoded video population exhausted");
 const auto index = observed_++;
 if (index < capacity_) return index;
 const auto slot = std::uniform_int_distribution<std::uint64_t>(0,index)(random);
 return slot < capacity_ ? std::optional{slot} : std::nullopt;
}
std::uint64_t PredictionReservoir::selected() const noexcept { return std::min(capacity_, observed_); }
}
