#include "src/backend/data/catalog/class_catalog.h"
#include <stdexcept>
#include <utility>
namespace mmltk::backend::data::catalog {
ClassCatalog::ClassCatalog(std::vector<std::string> names, std::size_t name_capacity) : names_(std::move(names)) {
 if (names_.size() > kClassCatalogCapacity || name_capacity > kClassNameCapacity) throw std::invalid_argument("class catalog exceeds supported capacity");
 lookup_.reserve(names_.size());
 for (std::size_t index = 0; index < names_.size(); ++index) {
  const auto& name = names_[index];
  if (name.empty() || name.size() > name_capacity || name.find('\0') != std::string::npos)
   throw std::invalid_argument("class catalog contains an empty, NUL-containing, or overlong name");
  if (!lookup_.emplace(name, static_cast<std::uint32_t>(index)).second) throw std::invalid_argument("class catalog contains duplicate name: " + name);
 }
}
ClassCatalog::ClassCatalog(const OrderedClassCatalog& record)
    : ClassCatalog([&] {
       if (record.names.size() > kClassCatalogCapacity) throw std::invalid_argument("class catalog exceeds supported capacity");
       std::vector<std::string> names;
       names.reserve(record.names.size());
       for (const auto& name : record.names) names.push_back(name.value);
       return names;
      }()) {}
std::optional<std::uint32_t> ClassCatalog::resolve(std::string_view name) const {
 const auto found = lookup_.find(name);
 if (found == lookup_.end()) return std::nullopt;
 return found->second;
}
std::vector<std::uint32_t> ClassCatalog::permutation_to(const ClassCatalog& destination) const {
 if (size() != destination.size()) throw std::invalid_argument("class catalogs have different foreground sets");
 std::vector<std::uint32_t> permutation;
 permutation.reserve(size());
 for (const auto& name : names_) {
  const auto index = destination.resolve(name);
  if (!index) throw std::invalid_argument("class catalogs have different foreground sets");
  permutation.push_back(*index);
 }
 return permutation;
}
OrderedClassCatalog ClassCatalog::record() const {
 OrderedClassCatalog result;
 result.names.reserve(size());
 for (const auto& name : names_) result.names.push_back({name});
 return result;
}
}  // namespace mmltk::backend::data::catalog
