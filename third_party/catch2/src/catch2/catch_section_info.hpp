

#ifndef CATCH_SECTION_INFO_HPP_INCLUDED
#define CATCH_SECTION_INFO_HPP_INCLUDED

#include <catch2/internal/catch_move_and_forward.hpp>
#include <catch2/internal/catch_source_line_info.hpp>
#include <catch2/internal/catch_stringref.hpp>
#include <catch2/catch_totals.hpp>

#include <string>

namespace Catch {

struct SectionInfo {
    SectionInfo(SourceLineInfo const& _lineInfo, std::string _name, const char* const = nullptr)
        : name(CATCH_MOVE(_name)), lineInfo(_lineInfo) {}

    std::string name;
    SourceLineInfo lineInfo;
};

struct SectionEndInfo {
    SectionInfo sectionInfo;
    Counts prevAssertions;
    double durationInSeconds;
};

}  

#endif  // CATCH_SECTION_INFO_HPP_INCLUDED
