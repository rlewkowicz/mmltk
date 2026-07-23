

#ifndef CATCH_LIST_HPP_INCLUDED
#define CATCH_LIST_HPP_INCLUDED

#include <catch2/internal/catch_stringref.hpp>

#include <set>
#include <string>

namespace Catch {

class IEventListener;
class Config;

struct ReporterDescription {
    std::string name, description;
};
struct ListenerDescription {
    StringRef name;
    std::string description;
};

struct TagInfo {
    void add(StringRef spelling);
    std::string all() const;

    std::set<StringRef> spellings;
    std::size_t count = 0;
};

bool list(IEventListener& reporter, Config const& config);

}  

#endif  // CATCH_LIST_HPP_INCLUDED
