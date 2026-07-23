

#ifndef CATCH_TAG_ALIAS_HPP_INCLUDED
#define CATCH_TAG_ALIAS_HPP_INCLUDED

#include <catch2/internal/catch_source_line_info.hpp>

#include <string>

namespace Catch {

struct TagAlias {
    TagAlias(std::string const& _tag, SourceLineInfo _lineInfo) : tag(_tag), lineInfo(_lineInfo) {}

    std::string tag;
    SourceLineInfo lineInfo;
};

}  

#endif  // CATCH_TAG_ALIAS_HPP_INCLUDED
